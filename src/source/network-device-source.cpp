#include "network-device-source.hpp"

#include "core/device-registry.hpp"
#include "onvif/onvif-auth.hpp"
#include "plugin-support.h"
#include "ui/device-explorer-dialog.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QApplication>
#include <QDialog>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaxQueuedAudioPackets = 12;

struct AudioPacket {
    uint32_t frames = 0;
    uint32_t samplesPerSec = 0;
    enum speaker_layout speakers = SPEAKERS_UNKNOWN;
    uint8_t planeCount = 0;
    uint64_t captureTimeNs = 0;
    std::array<std::vector<float>, MAX_AV_PLANES> planes;
};

constexpr const char *kPropExplorer = "open_explorer";
constexpr const char *kPropReconnect = "reconnect_stream";
constexpr const char *kPropInfoBanner = "info_banner";
constexpr const char *kPropDeviceName = "device_name";
constexpr const char *kPropManufacturer = "device_manufacturer";
constexpr const char *kPropModel = "device_model";
constexpr const char *kPropSerialNumber = "device_serial";
constexpr const char *kPropDeviceServiceUrl = "device_service_url";
constexpr const char *kPropMediaServiceUrl = "media_service_url";
constexpr const char *kPropPtzServiceUrl = "ptz_service_url";
constexpr const char *kPropUsername = "username";
constexpr const char *kPropPassword = "password";
constexpr const char *kPropProfileToken = "profile_token";
constexpr const char *kPropProfileName = "profile_name";
constexpr const char *kPropRtspUri = "rtsp_uri";
constexpr const char *kPropSnapshotUri = "snapshot_uri";
constexpr const char *kPropRtspTransport = "rtsp_transport";
constexpr const char *kPropLowLatency = "low_latency";
constexpr const char *kPropHwDecode = "hw_decode";
constexpr const char *kPropHasPtz = "has_ptz";
constexpr const char *kPropClockOffset = "clock_offset_secs";

struct NetworkDeviceSource {
    obs_source_t *source = nullptr;
    obs_source_t *mediaSource = nullptr;
    std::mutex mutex;
    std::atomic<bool> destroying{false};
    std::atomic<uint32_t> cachedWidth{0};
    std::atomic<uint32_t> cachedHeight{0};

    std::mutex audioQueueMutex;
    std::condition_variable audioQueueCv;
    std::deque<AudioPacket> audioQueue;
    bool audioStopRequested = false;
    bool audioResetPending = false;
    std::thread audioThread;

    QString sourceUuid;
    netdev::SourceConfig config;
    QString activeEffectiveUri;
    QString activeTransport;
    bool activeHwDecode = false;
    bool activeLowLatency = true;
};

QString safeSourceUuid(obs_source_t *source)
{
    const char *uuid = obs_source_get_uuid(source);
    if (uuid && uuid[0] != '\0') {
        return QString::fromUtf8(uuid);
    }
    return QString::asprintf("src_%p", static_cast<const void *>(source));
}

QString redactUriPassword(const QString &uri)
{
    QUrl u(uri);
    if (!u.isValid()) {
        return uri;
    }
    if (!u.password().isEmpty()) {
        u.setPassword(QStringLiteral("***"));
    }
    return u.toString();
}

void resetAudioQueue(NetworkDeviceSource *self)
{
    std::lock_guard<std::mutex> lock(self->audioQueueMutex);
    self->audioQueue.clear();
    self->audioResetPending = true;
}

void onChildAudioCaptured(void *param,
                          obs_source_t *childSource,
                          const struct audio_data *audioData,
                          bool muted)
{
    UNUSED_PARAMETER(childSource);
    auto *self = static_cast<NetworkDeviceSource *>(param);
    if (!self || self->destroying.load(std::memory_order_acquire) || !audioData ||
        audioData->frames == 0 || muted) {
        return;
    }

    struct obs_audio_info oai {};
    if (!obs_get_audio_info(&oai) || oai.samples_per_sec == 0) {
        return;
    }

    const size_t channels = get_audio_channels(oai.speakers);
    const size_t planeCount = std::min<size_t>(channels, MAX_AV_PLANES);
    if (planeCount == 0) {
        return;
    }

    AudioPacket pkt;
    pkt.frames = audioData->frames;
    pkt.samplesPerSec = oai.samples_per_sec;
    pkt.speakers = oai.speakers;
    pkt.planeCount = static_cast<uint8_t>(planeCount);
    pkt.captureTimeNs = os_gettime_ns();

    const size_t byteCount = static_cast<size_t>(audioData->frames) * sizeof(float);
    for (size_t i = 0; i < planeCount; ++i) {
        pkt.planes[i].resize(audioData->frames);
        if (audioData->data[i]) {
            std::memcpy(pkt.planes[i].data(), audioData->data[i], byteCount);
        } else {
            std::fill(pkt.planes[i].begin(), pkt.planes[i].end(), 0.0f);
        }
    }

    {
        std::lock_guard<std::mutex> lock(self->audioQueueMutex);
        if (self->audioStopRequested) {
            return;
        }
        while (self->audioQueue.size() >= kMaxQueuedAudioPackets) {
            self->audioQueue.pop_front();
        }
        self->audioQueue.push_back(std::move(pkt));
    }
    self->audioQueueCv.notify_one();
}

void audioForwardThreadLoop(NetworkDeviceSource *self)
{
    uint64_t virtualPts = 0;
    uint64_t lastWallNs = 0;
    uint64_t lastDurNs = 0;

    for (;;) {
        AudioPacket pkt;
        bool resetPending = false;
        {
            std::unique_lock<std::mutex> lock(self->audioQueueMutex);
            self->audioQueueCv.wait(lock, [self]() {
                return self->audioStopRequested || !self->audioQueue.empty();
            });
            if (self->audioStopRequested) {
                break;
            }
            resetPending = self->audioResetPending;
            self->audioResetPending = false;
            pkt = std::move(self->audioQueue.front());
            self->audioQueue.pop_front();
        }

        if (self->destroying.load(std::memory_order_acquire)) {
            break;
        }

        const uint64_t nowNs = pkt.captureTimeNs;
        const uint64_t durNs =
            (static_cast<uint64_t>(pkt.frames) * 1000000000ULL) / pkt.samplesPerSec;

        if (virtualPts == 0) {
            virtualPts = (nowNs > 20000000000ULL) ? 1000000000ULL : (nowNs + 20000000000ULL);
        } else if (resetPending || lastWallNs == 0 || nowNs < lastWallNs ||
                   (nowNs - lastWallNs) > 200000000ULL) {
            virtualPts += 3000000000ULL;
        } else {
            virtualPts += lastDurNs;
        }

        lastWallNs = nowNs;
        lastDurNs = durNs;

        struct obs_source_audio out {};
        for (uint8_t i = 0; i < pkt.planeCount; ++i) {
            out.data[i] = reinterpret_cast<const uint8_t *>(pkt.planes[i].data());
        }
        out.frames = pkt.frames;
        out.speakers = pkt.speakers;
        out.format = AUDIO_FORMAT_FLOAT_PLANAR;
        out.samples_per_sec = pkt.samplesPerSec;
        out.timestamp = virtualPts;

        obs_source_output_audio(self->source, &out);
    }
}

void refreshCachedDimensions(NetworkDeviceSource *self, obs_source_t *child)
{
    if (!self) {
        return;
    }
    if (child) {
        self->cachedWidth.store(obs_source_get_base_width(child), std::memory_order_relaxed);
        self->cachedHeight.store(obs_source_get_base_height(child), std::memory_order_relaxed);
    } else {
        self->cachedWidth.store(0, std::memory_order_relaxed);
        self->cachedHeight.store(0, std::memory_order_relaxed);
    }
}

void detachAndReleaseChild(NetworkDeviceSource *self)
{
    self->cachedWidth.store(0, std::memory_order_relaxed);
    self->cachedHeight.store(0, std::memory_order_relaxed);
    resetAudioQueue(self);

    obs_source_t *childToRelease = nullptr;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (self->mediaSource) {
            childToRelease = self->mediaSource;
            self->mediaSource = nullptr;
        }
        self->activeEffectiveUri.clear();
    }

    if (childToRelease) {
        obs_source_remove_audio_capture_callback(childToRelease, onChildAudioCaptured, self);
        obs_source_remove_active_child(self->source, childToRelease);
        obs_source_release(childToRelease);
    }
}

void pushSettingsToChild(NetworkDeviceSource *self,
                         const netdev::SourceConfig &cfg,
                         bool forceRestart = false)
{
    const QString effectiveUri = netdev::OnvifAuth::injectRtspCredentials(
        cfg.rtspUri, cfg.username, cfg.password);

    if (effectiveUri.isEmpty()) {
        detachAndReleaseChild(self);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (!forceRestart && self->mediaSource &&
            self->activeEffectiveUri == effectiveUri &&
            self->activeTransport == cfg.rtspTransport &&
            self->activeHwDecode == cfg.hwDecode &&
            self->activeLowLatency == cfg.lowLatency) {
            return;
        }
    }

    QString ffOpts = (cfg.rtspTransport == QStringLiteral("udp"))
                         ? QStringLiteral("rtsp_transport=udp timeout=5000000")
                         : QStringLiteral("rtsp_transport=tcp timeout=5000000");
    if (cfg.lowLatency) {
        ffOpts += QStringLiteral(
            " fflags=nobuffer flags=low_delay max_delay=50000"
            " reorder_queue_size=0 analyzeduration=500000 probesize=262144");
    }
    const QByteArray ffOptsUtf8 = ffOpts.toUtf8();

    obs_data_t *childData = obs_data_create();
    obs_data_set_bool(childData, "is_local_file", false);
    const QByteArray uriUtf8 = effectiveUri.toUtf8();
    obs_data_set_string(childData, "input", uriUtf8.constData());
    obs_data_set_bool(childData, "hw_decode", cfg.hwDecode);
    obs_data_set_int(childData, "buffering_mb", 0);
    obs_data_set_int(childData, "reconnect_delay_sec", 2);
    obs_data_set_bool(childData, "restart_on_activate", false);
    obs_data_set_bool(childData, "close_when_inactive", false);
    obs_data_set_bool(childData, "clear_on_media_end", false);
    obs_data_set_string(childData, "ffmpeg_options", ffOptsUtf8.constData());

    const QByteArray childName =
        QStringLiteral("%1_ffmpeg").arg(self->sourceUuid).toUtf8();
    obs_source_t *newChild =
        obs_source_create_private("ffmpeg_source", childName.constData(), childData);
    if (newChild) {
        obs_source_set_audio_mixers(newChild, 0);
        obs_source_set_async_unbuffered(newChild, cfg.lowLatency);
        obs_source_set_async_decoupled(newChild, cfg.lowLatency);
        obs_source_add_audio_capture_callback(newChild, onChildAudioCaptured, self);

        obs_source_t *oldChild = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            oldChild = self->mediaSource;
            self->mediaSource = newChild;
            self->activeEffectiveUri = effectiveUri;
            self->activeTransport = cfg.rtspTransport;
            self->activeHwDecode = cfg.hwDecode;
            self->activeLowLatency = cfg.lowLatency;
        }
        resetAudioQueue(self);
        obs_source_add_active_child(self->source, newChild);
        if (oldChild) {
            obs_source_remove_audio_capture_callback(oldChild, onChildAudioCaptured, self);
            obs_source_remove_active_child(self->source, oldChild);
            obs_source_release(oldChild);
        }
    }

    obs_data_release(childData);

    if (newChild) {
        const QString safeUri = redactUriPassword(effectiveUri);
        obs_log(LOG_INFO, "Configured RTSP stream (%s): %s [%s, hw_decode=%s, low_latency=%s]",
                self->sourceUuid.toUtf8().constData(),
                safeUri.toUtf8().constData(),
                ffOptsUtf8.constData(),
                cfg.hwDecode ? "true" : "false",
                cfg.lowLatency ? "true" : "false");
    }
}

void syncPtzRegistry(NetworkDeviceSource *self)
{
    if (self->config.hasPtz && !self->config.ptzServiceUrl.isEmpty() &&
        !self->config.profileToken.isEmpty()) {
        netdev::PtzCameraEntry entry;
        entry.sourceUuid = self->sourceUuid;
        const char *obsName = obs_source_get_name(self->source);
        entry.sourceName = obsName ? QString::fromUtf8(obsName) : self->config.deviceName;
        entry.deviceName = self->config.deviceName.isEmpty() ? entry.sourceName : self->config.deviceName;
        entry.manufacturer = self->config.manufacturer;
        entry.model = self->config.model;
        entry.serialNumber = self->config.serialNumber;
        entry.deviceServiceUrl = self->config.deviceServiceUrl;
        entry.ptzServiceUrl = self->config.ptzServiceUrl;
        entry.snapshotUri = self->config.snapshotUri;
        entry.profileToken = self->config.profileToken;
        entry.username = self->config.username;
        entry.password = self->config.password;
        entry.clockOffsetSecs = self->config.clockOffsetSecs;
        entry.fallbackHostKey = netdev::computeCameraFallbackHostKey(
            entry.deviceServiceUrl, entry.ptzServiceUrl, entry.deviceName);
        entry.cameraKey = netdev::computeCameraStorageKey(
            entry.manufacturer, entry.model, entry.serialNumber,
            entry.deviceServiceUrl, entry.ptzServiceUrl, entry.deviceName);
        netdev::DeviceRegistry::instance().registerCamera(entry);
    } else {
        netdev::DeviceRegistry::instance().unregisterCamera(self->sourceUuid);
    }
}

const char *networkDeviceGetName(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("NetworkDevice.SourceName");
}

void networkDeviceDefaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, kPropRtspTransport, "tcp");
    obs_data_set_default_bool(settings, kPropLowLatency, true);
    obs_data_set_default_bool(settings, kPropHwDecode, false);
    obs_data_set_default_bool(settings, kPropHasPtz, false);
    obs_data_set_default_int(settings, kPropClockOffset, 0);
}

void networkDeviceUpdate(void *data, obs_data_t *settings)
{
    auto *self = static_cast<NetworkDeviceSource *>(data);
    if (!self) {
        return;
    }

    const netdev::SourceConfig cfg = readSourceConfigFromData(settings);
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        self->config = cfg;
    }

    pushSettingsToChild(self, cfg);
    syncPtzRegistry(self);
}

void *networkDeviceCreate(obs_data_t *settings, obs_source_t *source)
{
    auto *self = new NetworkDeviceSource();
    self->source = source;
    self->sourceUuid = safeSourceUuid(source);
    obs_source_set_async_unbuffered(source, true);
    obs_source_set_async_decoupled(source, true);
    self->audioThread = std::thread(audioForwardThreadLoop, self);

    networkDeviceUpdate(self, settings);
    obs_log(LOG_INFO, "Created network_device_source (%s)",
            self->sourceUuid.toUtf8().constData());
    return self;
}

void networkDeviceDestroy(void *data)
{
    auto *self = static_cast<NetworkDeviceSource *>(data);
    if (!self) {
        return;
    }

    self->destroying.store(true, std::memory_order_release);
    netdev::DeviceRegistry::instance().unregisterCamera(self->sourceUuid);
    detachAndReleaseChild(self);

    {
        std::lock_guard<std::mutex> lock(self->audioQueueMutex);
        self->audioStopRequested = true;
        self->audioQueue.clear();
    }
    self->audioQueueCv.notify_all();
    if (self->audioThread.joinable()) {
        self->audioThread.join();
    }

    delete self;
}

bool onOpenExplorerClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);

    auto *self = static_cast<NetworkDeviceSource *>(data);
    if (!self) {
        return false;
    }

    obs_data_t *settings = obs_source_get_settings(self->source);
    const netdev::SourceConfig currentCfg = readSourceConfigFromData(settings);

    QWidget *parent = QApplication::activeWindow();
    if (!parent) {
        parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    }

    netdev::DeviceExplorerDialog dlg(currentCfg, parent);
    if (dlg.exec() == QDialog::Accepted) {
        const netdev::SourceConfig chosen = dlg.selectedConfig();
        applySourceConfigToData(settings, chosen);
        obs_source_update(self->source, settings);
        obs_data_release(settings);
        return true;
    }

    obs_data_release(settings);
    return false;
}

bool onReconnectClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);

    auto *self = static_cast<NetworkDeviceSource *>(data);
    if (!self) {
        return false;
    }

    netdev::SourceConfig cfg;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        cfg = self->config;
    }

    pushSettingsToChild(self, cfg, true);
    return false;
}

obs_properties_t *networkDeviceProperties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();
    obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

    obs_properties_add_button(props, kPropExplorer,
                              obs_module_text("NetworkDevice.ExplorerButton"),
                              onOpenExplorerClicked);

    obs_properties_add_text(props, kPropInfoBanner,
                            "Usa 'Explorar dispositivos de red...' para escanear tu red, "
                            "iniciar sesion, ver captura previa y elegir perfil ONVIF.",
                            OBS_TEXT_INFO);

    obs_properties_add_text(props, kPropDeviceName,
                            obs_module_text("NetworkDevice.DeviceName"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, kPropDeviceServiceUrl,
                            obs_module_text("NetworkDevice.ServiceUrl"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, kPropUsername,
                            obs_module_text("NetworkDevice.Username"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, kPropPassword,
                            obs_module_text("NetworkDevice.Password"), OBS_TEXT_PASSWORD);
    obs_properties_add_text(props, kPropRtspUri,
                            obs_module_text("NetworkDevice.RtspUri"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, kPropProfileToken,
                            obs_module_text("NetworkDevice.ProfileToken"), OBS_TEXT_DEFAULT);

    obs_property_t *transport = obs_properties_add_list(
        props, kPropRtspTransport, obs_module_text("NetworkDevice.RtspTransport"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(transport, obs_module_text("NetworkDevice.RtspTransport.Tcp"), "tcp");
    obs_property_list_add_string(transport, obs_module_text("NetworkDevice.RtspTransport.Udp"), "udp");

    obs_properties_add_bool(props, kPropLowLatency, obs_module_text("NetworkDevice.LowLatency"));
    obs_properties_add_bool(props, kPropHwDecode, obs_module_text("NetworkDevice.HwDecode"));
    obs_properties_add_bool(props, kPropHasPtz, obs_module_text("NetworkDevice.HasPtz"));
    obs_properties_add_text(props, kPropPtzServiceUrl,
                            obs_module_text("NetworkDevice.PtzServiceUrl"), OBS_TEXT_DEFAULT);

    obs_properties_add_button(props, kPropReconnect,
                              obs_module_text("NetworkDevice.ReconnectButton"),
                              onReconnectClicked);

    return props;
}

obs_source_t *acquireChild(NetworkDeviceSource *self)
{
    if (!self) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    return self->mediaSource ? obs_source_get_ref(self->mediaSource) : nullptr;
}

uint32_t networkDeviceGetWidth(void *data)
{
    const auto *self = static_cast<const NetworkDeviceSource *>(data);
    return self ? self->cachedWidth.load(std::memory_order_relaxed) : 0;
}

uint32_t networkDeviceGetHeight(void *data)
{
    const auto *self = static_cast<const NetworkDeviceSource *>(data);
    return self ? self->cachedHeight.load(std::memory_order_relaxed) : 0;
}

void networkDeviceVideoTick(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    auto *self = static_cast<NetworkDeviceSource *>(data);
    obs_source_t *child = acquireChild(self);
    refreshCachedDimensions(self, child);
    if (child) {
        obs_source_release(child);
    }
}

void networkDeviceVideoRender(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    auto *self = static_cast<NetworkDeviceSource *>(data);
    obs_source_t *child = acquireChild(self);
    if (!child) {
        return;
    }
    refreshCachedDimensions(self, child);
    obs_source_video_render(child);
    obs_source_release(child);
}

enum gs_color_space networkDeviceGetColorSpace(void *data,
                                               size_t count,
                                               const enum gs_color_space *preferredSpaces)
{
    obs_source_t *child = acquireChild(static_cast<NetworkDeviceSource *>(data));
    if (!child) {
        return GS_CS_SRGB;
    }
    const enum gs_color_space cs = obs_source_get_color_space(child, count, preferredSpaces);
    obs_source_release(child);
    return cs;
}

void networkDeviceEnumSources(void *data, obs_source_enum_proc_t enumCallback, void *param)
{
    auto *self = static_cast<NetworkDeviceSource *>(data);
    obs_source_t *child = acquireChild(self);
    if (!child) {
        return;
    }
    enumCallback(self->source, child, param);
    obs_source_release(child);
}

} // namespace

void applySourceConfigToData(obs_data_t *settings, const netdev::SourceConfig &cfg)
{
    if (!settings) {
        return;
    }
    obs_data_set_string(settings, kPropDeviceName, cfg.deviceName.toUtf8().constData());
    obs_data_set_string(settings, kPropManufacturer, cfg.manufacturer.toUtf8().constData());
    obs_data_set_string(settings, kPropModel, cfg.model.toUtf8().constData());
    obs_data_set_string(settings, kPropSerialNumber, cfg.serialNumber.toUtf8().constData());
    obs_data_set_string(settings, kPropDeviceServiceUrl, cfg.deviceServiceUrl.toUtf8().constData());
    obs_data_set_string(settings, kPropMediaServiceUrl, cfg.mediaServiceUrl.toUtf8().constData());
    obs_data_set_string(settings, kPropPtzServiceUrl, cfg.ptzServiceUrl.toUtf8().constData());
    obs_data_set_string(settings, kPropUsername, cfg.username.toUtf8().constData());
    obs_data_set_string(settings, kPropPassword, cfg.password.toUtf8().constData());
    obs_data_set_string(settings, kPropProfileToken, cfg.profileToken.toUtf8().constData());
    obs_data_set_string(settings, kPropProfileName, cfg.profileName.toUtf8().constData());
    obs_data_set_string(settings, kPropRtspUri, cfg.rtspUri.toUtf8().constData());
    obs_data_set_string(settings, kPropSnapshotUri, cfg.snapshotUri.toUtf8().constData());
    obs_data_set_string(settings, kPropRtspTransport, cfg.rtspTransport.toUtf8().constData());
    obs_data_set_bool(settings, kPropLowLatency, cfg.lowLatency);
    obs_data_set_bool(settings, kPropHwDecode, cfg.hwDecode);
    obs_data_set_bool(settings, kPropHasPtz, cfg.hasPtz);
    obs_data_set_int(settings, kPropClockOffset, cfg.clockOffsetSecs);
}

netdev::SourceConfig readSourceConfigFromData(obs_data_t *settings)
{
    netdev::SourceConfig cfg;
    if (!settings) {
        return cfg;
    }
    cfg.protocol = netdev::DeviceProtocol::Onvif;
    cfg.deviceName = QString::fromUtf8(obs_data_get_string(settings, kPropDeviceName));
    cfg.manufacturer = QString::fromUtf8(obs_data_get_string(settings, kPropManufacturer));
    cfg.model = QString::fromUtf8(obs_data_get_string(settings, kPropModel));
    cfg.serialNumber = QString::fromUtf8(obs_data_get_string(settings, kPropSerialNumber));
    cfg.deviceServiceUrl = QString::fromUtf8(obs_data_get_string(settings, kPropDeviceServiceUrl));
    cfg.mediaServiceUrl = QString::fromUtf8(obs_data_get_string(settings, kPropMediaServiceUrl));
    cfg.ptzServiceUrl = QString::fromUtf8(obs_data_get_string(settings, kPropPtzServiceUrl));
    cfg.username = QString::fromUtf8(obs_data_get_string(settings, kPropUsername));
    cfg.password = QString::fromUtf8(obs_data_get_string(settings, kPropPassword));
    cfg.profileToken = QString::fromUtf8(obs_data_get_string(settings, kPropProfileToken));
    cfg.profileName = QString::fromUtf8(obs_data_get_string(settings, kPropProfileName));
    cfg.rtspUri = QString::fromUtf8(obs_data_get_string(settings, kPropRtspUri));
    cfg.snapshotUri = QString::fromUtf8(obs_data_get_string(settings, kPropSnapshotUri));
    const QString transport = QString::fromUtf8(obs_data_get_string(settings, kPropRtspTransport));
    cfg.rtspTransport = transport.isEmpty() ? QStringLiteral("tcp") : transport;
    cfg.lowLatency = obs_data_get_bool(settings, kPropLowLatency);
    cfg.hwDecode = obs_data_get_bool(settings, kPropHwDecode);
    cfg.hasPtz = obs_data_get_bool(settings, kPropHasPtz);
    cfg.clockOffsetSecs = obs_data_get_int(settings, kPropClockOffset);
    return cfg;
}

void registerNetworkDeviceSource()
{
    struct obs_source_info info {};
    info.id = "network_device_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO |
                        OBS_SOURCE_SRGB;
    info.get_name = networkDeviceGetName;
    info.create = networkDeviceCreate;
    info.destroy = networkDeviceDestroy;
    info.get_defaults = networkDeviceDefaults;
    info.get_properties = networkDeviceProperties;
    info.update = networkDeviceUpdate;
    info.get_width = networkDeviceGetWidth;
    info.get_height = networkDeviceGetHeight;
    info.video_tick = networkDeviceVideoTick;
    info.video_render = networkDeviceVideoRender;
    info.enum_active_sources = networkDeviceEnumSources;
    info.enum_all_sources = networkDeviceEnumSources;
    info.video_get_color_space = networkDeviceGetColorSpace;
    info.icon_type = OBS_ICON_TYPE_CAMERA;

    obs_register_source(&info);
}
