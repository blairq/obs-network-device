#include "network-device-source.hpp"

#include "core/device-registry.hpp"
#include "core/http-snapshot-streamer.hpp"
#include "core/stream-options.hpp"
#include "onvif/onvif-auth.hpp"
#include "onvif/onvif-client.hpp"
#include "plugin-support.h"
#include "ui/device-explorer-dialog.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
constexpr const char *kPropOpenPtz = "open_ptz_dock";
constexpr const char *kPropRefreshProfiles = "refresh_profiles";
constexpr const char *kPropReconnect = "reconnect_stream";
constexpr const char *kPropInfoBanner = "info_banner";
constexpr const char *kPropDeviceName = "device_name";
constexpr const char *kPropManufacturer = "device_manufacturer";
constexpr const char *kPropModel = "device_model";
constexpr const char *kPropSerialNumber = "device_serial";
constexpr const char *kPropDeviceServiceUrl = "device_service_url";
constexpr const char *kPropMediaServiceUrl = "media_service_url";
constexpr const char *kPropMedia2ServiceUrl = "media2_service_url";
constexpr const char *kPropPtzServiceUrl = "ptz_service_url";
constexpr const char *kPropUsername = "username";
constexpr const char *kPropPassword = "password";
constexpr const char *kPropProfileSelector = "profile_selector";
constexpr const char *kPropLastAppliedSelector = "last_applied_profile_selector";
constexpr const char *kPropDetectedProfilesJson = "detected_profiles_json";
constexpr const char *kPropProfileToken = "profile_token";
constexpr const char *kPropProfileName = "profile_name";
constexpr const char *kPropRtspUri = "rtsp_uri";
constexpr const char *kPropSnapshotUri = "snapshot_uri";
constexpr const char *kPropRtspTransport = "rtsp_transport";
constexpr const char *kPropInputFormat = "input_format";
constexpr const char *kPropLatencyMode = "latency_mode";
constexpr const char *kPropBufferingMb = "buffering_mb";
constexpr const char *kPropLowLatency = "low_latency";
constexpr const char *kPropHwDecode = "hw_decode";
constexpr const char *kPropEnableAudio = "enable_audio";
constexpr const char *kPropSnapshotFps = "snapshot_fps";
constexpr const char *kPropExtraFfmpegOpts = "extra_ffmpeg_options";
constexpr const char *kPropHasPtz = "has_ptz";
constexpr const char *kPropClockOffset = "clock_offset_secs";

struct NetworkDeviceSource {
    obs_source_t *source = nullptr;
    obs_source_t *mediaSource = nullptr;
    bool childAudioCallbackAttached = false;
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
    QString activeInputFormat;
    QString activeFfmpegOptions;
    int activeBufferingMb = 0;
    bool activeHwDecode = false;
    bool activeEnableAudio = true;
    bool activeAsyncUnbuffered = true;
    bool activeSnapshotMode = false;
    int activeSnapshotFps = 15;

    std::atomic<bool> snapshotMode{false};
    netdev::HttpSnapshotStreamer snapshotStreamer;
    gs_texture_t *snapshotTex = nullptr;
    uint32_t snapshotTexW = 0;
    uint32_t snapshotTexH = 0;
    uint64_t uploadedSnapshotSeq = 0;
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
    bool detachAudio = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (self->mediaSource) {
            childToRelease = self->mediaSource;
            self->mediaSource = nullptr;
        }
        detachAudio = self->childAudioCallbackAttached;
        self->childAudioCallbackAttached = false;
        if (!self->activeSnapshotMode) {
            self->activeEffectiveUri.clear();
        }
        self->activeInputFormat.clear();
        self->activeFfmpegOptions.clear();
    }

    if (childToRelease) {
        if (detachAudio) {
            obs_source_remove_audio_capture_callback(childToRelease, onChildAudioCaptured, self);
        }
        obs_source_remove_active_child(self->source, childToRelease);
        obs_source_release(childToRelease);
    }
}

void pushSettingsToChild(NetworkDeviceSource *self,
                         const netdev::SourceConfig &cfg,
                         bool forceRestart = false)
{
    const netdev::ResolvedStreamParams resolved = netdev::buildResolvedStreamParams(cfg);

    if (resolved.effectiveUri.isEmpty()) {
        self->snapshotMode.store(false, std::memory_order_release);
        self->snapshotStreamer.stop();
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->activeSnapshotMode = false;
            self->activeEffectiveUri.clear();
        }
        detachAndReleaseChild(self);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (!forceRestart) {
            if (resolved.useHttpSnapshot) {
                if (self->activeSnapshotMode &&
                    self->activeEffectiveUri == resolved.effectiveUri &&
                    self->activeSnapshotFps == cfg.snapshotFps) {
                    return;
                }
            } else if (!self->activeSnapshotMode && self->mediaSource &&
                       self->activeEffectiveUri == resolved.effectiveUri &&
                       self->activeInputFormat == resolved.inputFormat &&
                       self->activeFfmpegOptions == resolved.ffmpegOptions &&
                       self->activeBufferingMb == resolved.bufferingMb &&
                       self->activeHwDecode == cfg.hwDecode &&
                       self->activeEnableAudio == cfg.enableAudio &&
                       self->activeAsyncUnbuffered == resolved.asyncUnbuffered) {
                return;
            }
        }
    }

    if (resolved.useHttpSnapshot) {
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->activeSnapshotMode = true;
            self->activeEffectiveUri = resolved.effectiveUri;
            self->activeSnapshotFps = cfg.snapshotFps;
            self->activeInputFormat.clear();
            self->activeFfmpegOptions.clear();
        }
        detachAndReleaseChild(self);
        self->snapshotMode.store(true, std::memory_order_release);
        self->snapshotStreamer.start(resolved.effectiveUri, cfg.username, cfg.password, cfg.snapshotFps);

        const QString safeUri = redactUriPassword(resolved.effectiveUri);
        obs_log(LOG_INFO, "Configured HTTP Snapshot stream (%s): %s [%d fps]",
                self->sourceUuid.toUtf8().constData(),
                safeUri.toUtf8().constData(),
                cfg.snapshotFps);
        return;
    }

    self->snapshotMode.store(false, std::memory_order_release);
    self->snapshotStreamer.stop();

    obs_source_set_async_unbuffered(self->source, resolved.asyncUnbuffered);
    obs_source_set_async_decoupled(self->source, resolved.asyncDecoupled);

    const QByteArray ffOptsUtf8 = resolved.ffmpegOptions.toUtf8();
    obs_data_t *childData = obs_data_create();
    obs_data_set_bool(childData, "is_local_file", false);
    const QByteArray uriUtf8 = resolved.effectiveUri.toUtf8();
    obs_data_set_string(childData, "input", uriUtf8.constData());
    if (!resolved.inputFormat.isEmpty()) {
        obs_data_set_string(childData, "input_format", resolved.inputFormat.toUtf8().constData());
    }
    obs_data_set_bool(childData, "hw_decode", cfg.hwDecode);
    obs_data_set_int(childData, "buffering_mb", resolved.bufferingMb);
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
        obs_source_set_async_unbuffered(newChild, resolved.asyncUnbuffered);
        obs_source_set_async_decoupled(newChild, resolved.asyncDecoupled);
        if (cfg.enableAudio) {
            obs_source_add_audio_capture_callback(newChild, onChildAudioCaptured, self);
        }

        obs_source_t *oldChild = nullptr;
        bool oldHadAudio = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            oldChild = self->mediaSource;
            oldHadAudio = self->childAudioCallbackAttached;
            self->mediaSource = newChild;
            self->childAudioCallbackAttached = cfg.enableAudio;
            self->activeSnapshotMode = false;
            self->activeEffectiveUri = resolved.effectiveUri;
            self->activeInputFormat = resolved.inputFormat;
            self->activeFfmpegOptions = resolved.ffmpegOptions;
            self->activeBufferingMb = resolved.bufferingMb;
            self->activeHwDecode = cfg.hwDecode;
            self->activeEnableAudio = cfg.enableAudio;
            self->activeAsyncUnbuffered = resolved.asyncUnbuffered;
        }
        resetAudioQueue(self);
        obs_source_add_active_child(self->source, newChild);
        if (oldChild) {
            if (oldHadAudio) {
                obs_source_remove_audio_capture_callback(oldChild, onChildAudioCaptured, self);
            }
            obs_source_remove_active_child(self->source, oldChild);
            obs_source_release(oldChild);
        }
    }

    obs_data_release(childData);

    if (newChild) {
        const QString safeUri = redactUriPassword(resolved.effectiveUri);
        obs_log(LOG_INFO,
                "Configured stream (%s): %s [fmt=%s, opts=%s, buf=%dMB, hw=%s, audio=%s]",
                self->sourceUuid.toUtf8().constData(),
                safeUri.toUtf8().constData(),
                resolved.inputFormat.isEmpty() ? "auto" : resolved.inputFormat.toUtf8().constData(),
                ffOptsUtf8.constData(),
                resolved.bufferingMb,
                cfg.hwDecode ? "true" : "false",
                cfg.enableAudio ? "true" : "false");
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
    obs_data_set_default_string(settings, kPropLatencyMode, "ultra");
    obs_data_set_default_string(settings, kPropInputFormat, "");
    obs_data_set_default_int(settings, kPropBufferingMb, 0);
    obs_data_set_default_bool(settings, kPropLowLatency, true);
    obs_data_set_default_bool(settings, kPropHwDecode, false);
    obs_data_set_default_bool(settings, kPropEnableAudio, true);
    obs_data_set_default_int(settings, kPropSnapshotFps, 15);
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
    self->snapshotMode.store(false, std::memory_order_release);
    self->snapshotStreamer.stop();
    netdev::DeviceRegistry::instance().unregisterCamera(self->sourceUuid);
    detachAndReleaseChild(self);

    if (self->snapshotTex) {
        obs_enter_graphics();
        gs_texture_destroy(self->snapshotTex);
        obs_leave_graphics();
        self->snapshotTex = nullptr;
        self->snapshotTexW = 0;
        self->snapshotTexH = 0;
    }

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

void populateProfileSelectorProperty(obs_property_t *prop,
                                     const QVector<netdev::MediaProfile> &profiles)
{
    if (!prop) {
        return;
    }
    obs_property_list_clear(prop);
    const QVector<netdev::StreamOptionEntry> entries =
        netdev::buildSelectableStreamOptions(profiles);
    for (const netdev::StreamOptionEntry &e : entries) {
        obs_property_list_add_string(
            prop, e.label.toUtf8().constData(), e.id.toUtf8().constData());
    }
    obs_property_list_add_string(
        prop, obs_module_text("NetworkDevice.ProfileSelector.Custom"), "__custom__");
}

void updatePropertyVisibility(obs_properties_t *props, obs_data_t *settings)
{
    if (!props || !settings) {
        return;
    }
    const QString transport =
        QString::fromUtf8(obs_data_get_string(settings, kPropRtspTransport)).trimmed().toLower();
    const bool isSnap = (transport == QLatin1String("http_snapshot"));

    auto setVis = [props](const char *name, bool vis) {
        if (obs_property_t *p = obs_properties_get(props, name)) {
            obs_property_set_visible(p, vis);
        }
    };

    setVis(kPropSnapshotFps, isSnap);
    setVis(kPropInputFormat, !isSnap);
    setVis(kPropLatencyMode, !isSnap);
    setVis(kPropBufferingMb, !isSnap);
    setVis(kPropHwDecode, !isSnap);
    setVis(kPropEnableAudio, !isSnap);
    setVis(kPropExtraFfmpegOpts, !isSnap);
}

bool onStreamModeModified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    UNUSED_PARAMETER(property);
    updatePropertyVisibility(props, settings);
    return true;
}

bool onProfileSelectorModified(obs_properties_t *props,
                               obs_property_t *property,
                               obs_data_t *settings)
{
    UNUSED_PARAMETER(property);
    if (!settings) {
        return false;
    }

    const QString selectedId =
        QString::fromUtf8(obs_data_get_string(settings, kPropProfileSelector));
    const QString lastApplied =
        QString::fromUtf8(obs_data_get_string(settings, kPropLastAppliedSelector));
    if (selectedId.isEmpty() || selectedId == QLatin1String("__custom__") ||
        selectedId == lastApplied) {
        return false;
    }

    const QVector<netdev::MediaProfile> profiles = netdev::deserializeMediaProfiles(
        QString::fromUtf8(obs_data_get_string(settings, kPropDetectedProfilesJson)));
    const QVector<netdev::StreamOptionEntry> entries =
        netdev::buildSelectableStreamOptions(profiles);

    for (const netdev::StreamOptionEntry &e : entries) {
        if (e.id != selectedId) {
            continue;
        }
        obs_data_set_string(settings, kPropProfileToken, e.profileToken.toUtf8().constData());
        obs_data_set_string(settings, kPropProfileName, e.profileName.toUtf8().constData());
        if (!e.snapshotUri.isEmpty()) {
            obs_data_set_string(settings, kPropSnapshotUri, e.snapshotUri.toUtf8().constData());
        }
        if (!e.uri.isEmpty()) {
            obs_data_set_string(settings, kPropRtspUri, e.uri.toUtf8().constData());
        }

        const QString curTransport =
            QString::fromUtf8(obs_data_get_string(settings, kPropRtspTransport)).trimmed().toLower();
        if (!e.suggestedTransport.isEmpty()) {
            obs_data_set_string(
                settings, kPropRtspTransport, e.suggestedTransport.toUtf8().constData());
        } else if (curTransport == QLatin1String("http_snapshot") ||
                   curTransport == QLatin1String("http_mjpeg")) {
            obs_data_set_string(settings, kPropRtspTransport, "udp");
        }

        if (!e.suggestedInputFormat.isEmpty()) {
            obs_data_set_string(
                settings, kPropInputFormat, e.suggestedInputFormat.toUtf8().constData());
        } else {
            obs_data_set_string(settings, kPropInputFormat, "");
        }

        obs_data_set_string(settings, kPropLastAppliedSelector, selectedId.toUtf8().constData());
        updatePropertyVisibility(props, settings);
        return true;
    }

    return false;
}

bool onOpenExplorerClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
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
        if (obs_property_t *selProp = obs_properties_get(props, kPropProfileSelector)) {
            populateProfileSelectorProperty(selProp, chosen.detectedProfiles);
        }
        const auto options = netdev::buildSelectableStreamOptions(chosen.detectedProfiles);
        const QString matched = netdev::findMatchingStreamOptionId(options, chosen);
        obs_data_set_string(settings, kPropProfileSelector, matched.toUtf8().constData());
        obs_data_set_string(settings, kPropLastAppliedSelector, matched.toUtf8().constData());
        updatePropertyVisibility(props, settings);
        obs_source_update(self->source, settings);
        obs_data_release(settings);
        return true;
    }

    obs_data_release(settings);
    return false;
}

bool onOpenPtzDockClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);

    auto *self = static_cast<NetworkDeviceSource *>(data);
    showNetworkDevicePtzDock(self ? self->sourceUuid : QString());
    return false;
}

bool onRefreshProfilesClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(property);

    auto *self = static_cast<NetworkDeviceSource *>(data);
    if (!self || self->destroying.load(std::memory_order_acquire)) {
        return false;
    }

    obs_data_t *settings = obs_source_get_settings(self->source);
    if (!settings) {
        return false;
    }
    const netdev::SourceConfig cfg = readSourceConfigFromData(settings);
    if (cfg.deviceServiceUrl.trimmed().isEmpty()) {
        obs_log(LOG_WARNING, "Cannot detect profiles: device_service_url is empty.");
        obs_data_release(settings);
        return false;
    }

    netdev::OnvifClient client;
    QEventLoop loop;
    netdev::DiscoveredDevice inspectedDev;
    bool inspectedOk = false;

    QObject::connect(&client, &netdev::OnvifClient::deviceInspected, &loop,
                     [&](const netdev::DiscoveredDevice &dev) {
                         inspectedDev = dev;
                         inspectedOk = true;
                         loop.quit();
                     });
    QObject::connect(&client, &netdev::OnvifClient::authRequired, &loop,
                     [&](const QString &, const QString &msg) {
                         obs_log(LOG_WARNING, "ONVIF auth error while detecting profiles: %s",
                                 msg.toUtf8().constData());
                         loop.quit();
                     });
    QObject::connect(&client, &netdev::OnvifClient::inspectionFailed, &loop,
                     [&](const QString &, const QString &err) {
                         obs_log(LOG_WARNING, "ONVIF profile detection failed: %s",
                                 err.toUtf8().constData());
                         loop.quit();
                     });

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(8000);

    client.inspectDevice(cfg.deviceServiceUrl, cfg.username, cfg.password);
    loop.exec();

    if (inspectedOk && !self->destroying.load(std::memory_order_acquire)) {
        const QString json = netdev::serializeMediaProfiles(inspectedDev.profiles);
        obs_data_set_string(settings, kPropDetectedProfilesJson, json.toUtf8().constData());
        if (!inspectedDev.mediaServiceUrl.isEmpty()) {
            obs_data_set_string(settings, kPropMediaServiceUrl,
                                inspectedDev.mediaServiceUrl.toUtf8().constData());
        }
        if (!inspectedDev.media2ServiceUrl.isEmpty()) {
            obs_data_set_string(settings, kPropMedia2ServiceUrl,
                                inspectedDev.media2ServiceUrl.toUtf8().constData());
        }
        if (!inspectedDev.ptzServiceUrl.isEmpty()) {
            obs_data_set_string(settings, kPropPtzServiceUrl,
                                inspectedDev.ptzServiceUrl.toUtf8().constData());
        }
        if (!inspectedDev.manufacturer.isEmpty()) {
            obs_data_set_string(settings, kPropManufacturer,
                                inspectedDev.manufacturer.toUtf8().constData());
        }
        if (!inspectedDev.model.isEmpty()) {
            obs_data_set_string(settings, kPropModel, inspectedDev.model.toUtf8().constData());
        }
        if (!inspectedDev.serialNumber.isEmpty()) {
            obs_data_set_string(settings, kPropSerialNumber,
                                inspectedDev.serialNumber.toUtf8().constData());
        }
        obs_data_set_int(settings, kPropClockOffset, inspectedDev.clockOffsetSecs);

        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->config.detectedProfiles = inspectedDev.profiles;
        }

        if (obs_property_t *selProp = obs_properties_get(props, kPropProfileSelector)) {
            populateProfileSelectorProperty(selProp, inspectedDev.profiles);
        }
        const auto options = netdev::buildSelectableStreamOptions(inspectedDev.profiles);
        const QString matched = netdev::findMatchingStreamOptionId(options, readSourceConfigFromData(settings));
        obs_data_set_string(settings, kPropProfileSelector, matched.toUtf8().constData());
        obs_data_set_string(settings, kPropLastAppliedSelector, matched.toUtf8().constData());
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
    auto *self = static_cast<NetworkDeviceSource *>(data);
    obs_properties_t *props = obs_properties_create();
    obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

    obs_properties_add_button(props, kPropExplorer,
                              obs_module_text("NetworkDevice.ExplorerButton"),
                              onOpenExplorerClicked);
    obs_properties_add_button(props, kPropOpenPtz,
                              obs_module_text("NetworkDevice.OpenPtzButton"),
                              onOpenPtzDockClicked);
    obs_properties_add_button(props, kPropRefreshProfiles,
                              obs_module_text("NetworkDevice.RefreshProfilesButton"),
                              onRefreshProfilesClicked);

    obs_properties_add_text(
        props, kPropInfoBanner,
        "Tip baja latencia: los perfiles MJPEG (RTSP UDP o Snapshot HTTP continuo) o H.264 con GPU "
        "y audio desactivado suelen bajar el retardo al minimo.",
        OBS_TEXT_INFO);

    obs_property_t *profileSel = obs_properties_add_list(
        props, kPropProfileSelector, obs_module_text("NetworkDevice.ProfileSelector"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    {
        QVector<netdev::MediaProfile> profiles;
        if (self) {
            std::lock_guard<std::mutex> lock(self->mutex);
            profiles = self->config.detectedProfiles;
        }
        populateProfileSelectorProperty(profileSel, profiles);
    }
    obs_property_set_modified_callback(profileSel, onProfileSelectorModified);

    obs_property_t *transport = obs_properties_add_list(
        props, kPropRtspTransport, obs_module_text("NetworkDevice.RtspTransport"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(transport, obs_module_text("NetworkDevice.RtspTransport.Udp"), "udp");
    obs_property_list_add_string(transport, obs_module_text("NetworkDevice.RtspTransport.Tcp"), "tcp");
    obs_property_list_add_string(
        transport, obs_module_text("NetworkDevice.RtspTransport.UdpMulticast"), "udp_multicast");
    obs_property_list_add_string(transport, obs_module_text("NetworkDevice.RtspTransport.Http"), "http");
    obs_property_list_add_string(transport, obs_module_text("NetworkDevice.RtspTransport.Auto"), "auto");
    obs_property_list_add_string(
        transport, obs_module_text("NetworkDevice.RtspTransport.HttpMjpeg"), "http_mjpeg");
    obs_property_list_add_string(
        transport, obs_module_text("NetworkDevice.RtspTransport.HttpSnapshot"), "http_snapshot");
    obs_property_set_modified_callback(transport, onStreamModeModified);

    obs_property_t *latency = obs_properties_add_list(
        props, kPropLatencyMode, obs_module_text("NetworkDevice.LatencyMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(latency, obs_module_text("NetworkDevice.LatencyMode.Zero"), "zero");
    obs_property_list_add_string(latency, obs_module_text("NetworkDevice.LatencyMode.Ultra"), "ultra");
    obs_property_list_add_string(latency, obs_module_text("NetworkDevice.LatencyMode.Balanced"), "balanced");
    obs_property_list_add_string(latency, obs_module_text("NetworkDevice.LatencyMode.Standard"), "standard");

    obs_property_t *fmt = obs_properties_add_list(
        props, kPropInputFormat, obs_module_text("NetworkDevice.InputFormat"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(fmt, obs_module_text("NetworkDevice.InputFormat.Auto"), "");
    obs_property_list_add_string(fmt, "rtsp", "rtsp");
    obs_property_list_add_string(fmt, "mpjpeg (HTTP MJPEG)", "mpjpeg");
    obs_property_list_add_string(fmt, "mjpeg", "mjpeg");
    obs_property_list_add_string(fmt, "h264", "h264");
    obs_property_list_add_string(fmt, "hevc", "hevc");
    obs_property_list_add_string(fmt, "mpegts", "mpegts");
    obs_property_list_add_string(fmt, "sdp", "sdp");

    obs_property_t *bufProp = obs_properties_add_int_slider(
        props, kPropBufferingMb, obs_module_text("NetworkDevice.BufferingMb"), 0, 8, 1);
    obs_property_int_set_suffix(bufProp, " MB");

    obs_property_t *snapFpsProp = obs_properties_add_int_slider(
        props, kPropSnapshotFps, obs_module_text("NetworkDevice.SnapshotFps"), 1, 30, 1);
    obs_property_int_set_suffix(snapFpsProp, " fps");

    obs_property_t *hwProp = obs_properties_add_bool(
        props, kPropHwDecode, obs_module_text("NetworkDevice.HwDecode"));
    obs_property_set_long_description(hwProp, obs_module_text("NetworkDevice.HwDecode.ToolTip"));

    obs_property_t *audioProp = obs_properties_add_bool(
        props, kPropEnableAudio, obs_module_text("NetworkDevice.EnableAudio"));
    obs_property_set_long_description(audioProp, obs_module_text("NetworkDevice.EnableAudio.ToolTip"));

    obs_properties_add_text(props, kPropExtraFfmpegOpts,
                            obs_module_text("NetworkDevice.ExtraFfmpegOptions"), OBS_TEXT_DEFAULT);

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
    obs_properties_add_text(props, kPropSnapshotUri,
                            obs_module_text("NetworkDevice.SnapshotUri"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, kPropProfileToken,
                            obs_module_text("NetworkDevice.ProfileToken"), OBS_TEXT_DEFAULT);

    obs_properties_add_bool(props, kPropHasPtz, obs_module_text("NetworkDevice.HasPtz"));
    obs_properties_add_text(props, kPropPtzServiceUrl,
                            obs_module_text("NetworkDevice.PtzServiceUrl"), OBS_TEXT_DEFAULT);

    obs_properties_add_button(props, kPropReconnect,
                              obs_module_text("NetworkDevice.ReconnectButton"),
                              onReconnectClicked);

    if (self && self->source) {
        if (obs_data_t *curSettings = obs_source_get_settings(self->source)) {
            const QString curSel =
                QString::fromUtf8(obs_data_get_string(curSettings, kPropProfileSelector));
            if (curSel.isEmpty()) {
                netdev::SourceConfig cfgCopy;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    cfgCopy = self->config;
                }
                const auto opts = netdev::buildSelectableStreamOptions(cfgCopy.detectedProfiles);
                const QString matched = netdev::findMatchingStreamOptionId(opts, cfgCopy);
                obs_data_set_string(curSettings, kPropProfileSelector, matched.toUtf8().constData());
                obs_data_set_string(curSettings, kPropLastAppliedSelector, matched.toUtf8().constData());
            }
            updatePropertyVisibility(props, curSettings);
            obs_data_release(curSettings);
        }
    }

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
    if (!self) {
        return;
    }
    if (self->snapshotMode.load(std::memory_order_acquire)) {
        uint32_t sw = 0;
        uint32_t sh = 0;
        self->snapshotStreamer.latestDimensions(sw, sh);
        self->cachedWidth.store(sw, std::memory_order_relaxed);
        self->cachedHeight.store(sh, std::memory_order_relaxed);
        return;
    }
    obs_source_t *child = acquireChild(self);
    refreshCachedDimensions(self, child);
    if (child) {
        obs_source_release(child);
    }
}

void drawSnapshotFrame(NetworkDeviceSource *self)
{
    netdev::SnapshotFrame frame;
    if (self->snapshotStreamer.takeLatestFrameIfNewer(self->uploadedSnapshotSeq, frame) &&
        frame.width > 0 && frame.height > 0 && !frame.rgbaPixels.isEmpty()) {
        const uint8_t *dataPtr =
            reinterpret_cast<const uint8_t *>(frame.rgbaPixels.constData());
        if (!self->snapshotTex || self->snapshotTexW != frame.width ||
            self->snapshotTexH != frame.height) {
            if (self->snapshotTex) {
                gs_texture_destroy(self->snapshotTex);
                self->snapshotTex = nullptr;
            }
            self->snapshotTex = gs_texture_create(
                frame.width, frame.height, GS_RGBA, 1, &dataPtr, GS_DYNAMIC);
            self->snapshotTexW = frame.width;
            self->snapshotTexH = frame.height;
        } else {
            gs_texture_set_image(self->snapshotTex, dataPtr, frame.width * 4u, false);
        }
        self->uploadedSnapshotSeq = frame.seq;
        self->cachedWidth.store(frame.width, std::memory_order_relaxed);
        self->cachedHeight.store(frame.height, std::memory_order_relaxed);
    }

    if (self->snapshotTex && self->snapshotTexW > 0 && self->snapshotTexH > 0) {
        gs_effect_t *defaultEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        if (defaultEffect) {
            const bool linearSrgb = gs_get_linear_srgb();
            const bool prevSrgb = gs_framebuffer_srgb_enabled();
            gs_enable_framebuffer_srgb(linearSrgb);

            gs_eparam_t *imageParam = gs_effect_get_param_by_name(defaultEffect, "image");
            if (linearSrgb) {
                gs_effect_set_texture_srgb(imageParam, self->snapshotTex);
            } else {
                gs_effect_set_texture(imageParam, self->snapshotTex);
            }

            while (gs_effect_loop(defaultEffect, "Draw")) {
                gs_draw_sprite(self->snapshotTex, 0, self->snapshotTexW, self->snapshotTexH);
            }
            gs_enable_framebuffer_srgb(prevSrgb);
        }
    }
}

void networkDeviceVideoRender(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    auto *self = static_cast<NetworkDeviceSource *>(data);
    if (!self) {
        return;
    }

    if (self->snapshotMode.load(std::memory_order_acquire)) {
        drawSnapshotFrame(self);
        return;
    }

    if (self->snapshotTex) {
        gs_texture_destroy(self->snapshotTex);
        self->snapshotTex = nullptr;
        self->snapshotTexW = 0;
        self->snapshotTexH = 0;
        self->uploadedSnapshotSeq = 0;
    }

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
    obs_data_set_string(settings, kPropMedia2ServiceUrl, cfg.media2ServiceUrl.toUtf8().constData());
    obs_data_set_string(settings, kPropPtzServiceUrl, cfg.ptzServiceUrl.toUtf8().constData());
    obs_data_set_string(settings, kPropUsername, cfg.username.toUtf8().constData());
    obs_data_set_string(settings, kPropPassword, cfg.password.toUtf8().constData());
    obs_data_set_string(settings, kPropProfileToken, cfg.profileToken.toUtf8().constData());
    obs_data_set_string(settings, kPropProfileName, cfg.profileName.toUtf8().constData());
    obs_data_set_string(settings, kPropRtspUri, cfg.rtspUri.toUtf8().constData());
    obs_data_set_string(settings, kPropSnapshotUri, cfg.snapshotUri.toUtf8().constData());
    obs_data_set_string(settings, kPropRtspTransport, cfg.rtspTransport.toUtf8().constData());
    obs_data_set_string(settings, kPropInputFormat, cfg.inputFormat.toUtf8().constData());
    const QString lat = cfg.latencyMode.trimmed().isEmpty()
                            ? (cfg.lowLatency ? QStringLiteral("ultra") : QStringLiteral("standard"))
                            : cfg.latencyMode.trimmed();
    obs_data_set_string(settings, kPropLatencyMode, lat.toUtf8().constData());
    obs_data_set_int(settings, kPropBufferingMb, std::clamp(cfg.bufferingMb, 0, 8));
    obs_data_set_bool(settings, kPropLowLatency, lat != QLatin1String("standard"));
    obs_data_set_bool(settings, kPropHwDecode, cfg.hwDecode);
    obs_data_set_bool(settings, kPropEnableAudio, cfg.enableAudio);
    obs_data_set_int(settings, kPropSnapshotFps, std::clamp(cfg.snapshotFps, 1, 30));
    obs_data_set_string(settings, kPropExtraFfmpegOpts, cfg.extraFfmpegOptions.toUtf8().constData());
    obs_data_set_bool(settings, kPropHasPtz, cfg.hasPtz);
    obs_data_set_int(settings, kPropClockOffset, cfg.clockOffsetSecs);
    obs_data_set_string(
        settings, kPropDetectedProfilesJson,
        netdev::serializeMediaProfiles(cfg.detectedProfiles).toUtf8().constData());
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
    cfg.media2ServiceUrl = QString::fromUtf8(obs_data_get_string(settings, kPropMedia2ServiceUrl));
    cfg.ptzServiceUrl = QString::fromUtf8(obs_data_get_string(settings, kPropPtzServiceUrl));
    cfg.username = QString::fromUtf8(obs_data_get_string(settings, kPropUsername));
    cfg.password = QString::fromUtf8(obs_data_get_string(settings, kPropPassword));
    cfg.profileToken = QString::fromUtf8(obs_data_get_string(settings, kPropProfileToken));
    cfg.profileName = QString::fromUtf8(obs_data_get_string(settings, kPropProfileName));
    cfg.rtspUri = QString::fromUtf8(obs_data_get_string(settings, kPropRtspUri));
    cfg.snapshotUri = QString::fromUtf8(obs_data_get_string(settings, kPropSnapshotUri));
    const QString transport = QString::fromUtf8(obs_data_get_string(settings, kPropRtspTransport));
    cfg.rtspTransport = transport.isEmpty() ? QStringLiteral("tcp") : transport;
    cfg.inputFormat = QString::fromUtf8(obs_data_get_string(settings, kPropInputFormat));
    cfg.lowLatency = obs_data_get_bool(settings, kPropLowLatency);
    const QString rawLat = QString::fromUtf8(obs_data_get_string(settings, kPropLatencyMode)).trimmed();
    if (!rawLat.isEmpty() &&
        (obs_data_has_user_value(settings, kPropLatencyMode) ||
         obs_data_has_default_value(settings, kPropLatencyMode))) {
        cfg.latencyMode = rawLat;
        cfg.lowLatency = (rawLat != QLatin1String("standard"));
    } else {
        cfg.latencyMode = cfg.lowLatency ? QStringLiteral("ultra") : QStringLiteral("standard");
    }
    cfg.bufferingMb = std::clamp(static_cast<int>(obs_data_get_int(settings, kPropBufferingMb)), 0, 8);
    cfg.hwDecode = obs_data_get_bool(settings, kPropHwDecode);
    if (obs_data_has_user_value(settings, kPropEnableAudio) ||
        obs_data_has_default_value(settings, kPropEnableAudio)) {
        cfg.enableAudio = obs_data_get_bool(settings, kPropEnableAudio);
    } else {
        cfg.enableAudio = true;
    }
    const int rawSnapFps = static_cast<int>(obs_data_get_int(settings, kPropSnapshotFps));
    cfg.snapshotFps = (rawSnapFps <= 0) ? 15 : std::clamp(rawSnapFps, 1, 30);
    cfg.extraFfmpegOptions = QString::fromUtf8(obs_data_get_string(settings, kPropExtraFfmpegOpts));
    cfg.hasPtz = obs_data_get_bool(settings, kPropHasPtz);
    cfg.clockOffsetSecs = obs_data_get_int(settings, kPropClockOffset);
    cfg.detectedProfiles = netdev::deserializeMediaProfiles(
        QString::fromUtf8(obs_data_get_string(settings, kPropDetectedProfilesJson)));
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
