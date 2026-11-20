#include "network-device-source.hpp"

#include "core/device-registry.hpp"
#include "onvif/onvif-auth.hpp"
#include "plugin-support.h"
#include "ui/device-explorer-dialog.hpp"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QDialog>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <cstring>
#include <mutex>

namespace {

constexpr const char *kPropExplorer = "open_explorer";
constexpr const char *kPropReconnect = "reconnect_stream";
constexpr const char *kPropInfoBanner = "info_banner";
constexpr const char *kPropDeviceName = "device_name";
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
constexpr const char *kPropHwDecode = "hw_decode";
constexpr const char *kPropHasPtz = "has_ptz";
constexpr const char *kPropClockOffset = "clock_offset_secs";

struct NetworkDeviceSource {
    obs_source_t *source = nullptr;
    obs_source_t *mediaSource = nullptr;
    std::mutex mutex;

    QString sourceUuid;
    netdev::SourceConfig config;
    QString activeEffectiveUri;
    QString activeTransport;
    bool activeHwDecode = true;
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

void detachAndReleaseChild(NetworkDeviceSource *self)
{
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
        obs_source_remove_active_child(self->source, childToRelease);
        obs_source_release(childToRelease);
    }
}

void pushSettingsToChild(NetworkDeviceSource *self, const netdev::SourceConfig &cfg)
{
    const QString effectiveUri = netdev::OnvifAuth::injectRtspCredentials(
        cfg.rtspUri, cfg.username, cfg.password);

    if (effectiveUri.isEmpty()) {
        detachAndReleaseChild(self);
        return;
    }

    obs_data_t *childData = obs_data_create();
    obs_data_set_bool(childData, "is_local_file", false);
    const QByteArray uriUtf8 = effectiveUri.toUtf8();
    obs_data_set_string(childData, "input", uriUtf8.constData());
    obs_data_set_bool(childData, "hw_decode", cfg.hwDecode);
    obs_data_set_int(childData, "buffering_mb", 0);
    obs_data_set_int(childData, "reconnect_delay_sec", 2);
    obs_data_set_bool(childData, "restart_on_activate", true);
    obs_data_set_bool(childData, "clear_on_media_end", false);

    const char *ffOpts = (cfg.rtspTransport == QStringLiteral("udp"))
                             ? "rtsp_transport=udp"
                             : "rtsp_transport=tcp";
    obs_data_set_string(childData, "ffmpeg_options", ffOpts);

    obs_source_t *existingChild = nullptr;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (self->mediaSource) {
            existingChild = obs_source_get_ref(self->mediaSource);
        }
        self->activeEffectiveUri = effectiveUri;
        self->activeTransport = cfg.rtspTransport;
        self->activeHwDecode = cfg.hwDecode;
    }

    if (existingChild) {
        obs_source_update(existingChild, childData);
        obs_source_release(existingChild);
    } else {
        const QByteArray childName =
            QStringLiteral("%1_ffmpeg").arg(self->sourceUuid).toUtf8();
        obs_source_t *newChild =
            obs_source_create_private("ffmpeg_source", childName.constData(), childData);
        if (newChild) {
            obs_source_t *oldChild = nullptr;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                oldChild = self->mediaSource;
                self->mediaSource = newChild;
            }
            obs_source_add_active_child(self->source, newChild);
            if (oldChild) {
                obs_source_remove_active_child(self->source, oldChild);
                obs_source_release(oldChild);
            }
        }
    }

    obs_data_release(childData);

    const QString safeUri = redactUriPassword(effectiveUri);
    obs_log(LOG_INFO, "Configured RTSP stream (%s): %s [%s, hw_decode=%s]",
            self->sourceUuid.toUtf8().constData(),
            safeUri.toUtf8().constData(),
            ffOpts,
            cfg.hwDecode ? "true" : "false");
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
        entry.ptzServiceUrl = self->config.ptzServiceUrl;
        entry.profileToken = self->config.profileToken;
        entry.username = self->config.username;
        entry.password = self->config.password;
        entry.clockOffsetSecs = self->config.clockOffsetSecs;
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
    obs_data_set_default_bool(settings, kPropHwDecode, true);
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

    netdev::DeviceRegistry::instance().unregisterCamera(self->sourceUuid);
    detachAndReleaseChild(self);
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

    detachAndReleaseChild(self);
    pushSettingsToChild(self, cfg);
    return false;
}

obs_properties_t *networkDeviceProperties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

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
    obs_source_t *child = acquireChild(static_cast<NetworkDeviceSource *>(data));
    if (!child) {
        return 0;
    }
    const uint32_t w = obs_source_get_width(child);
    obs_source_release(child);
    return w;
}

uint32_t networkDeviceGetHeight(void *data)
{
    obs_source_t *child = acquireChild(static_cast<NetworkDeviceSource *>(data));
    if (!child) {
        return 0;
    }
    const uint32_t h = obs_source_get_height(child);
    obs_source_release(child);
    return h;
}

void networkDeviceVideoRender(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    obs_source_t *child = acquireChild(static_cast<NetworkDeviceSource *>(data));
    if (!child) {
        return;
    }
    obs_source_video_render(child);
    obs_source_release(child);
}

bool networkDeviceAudioRender(void *data,
                              uint64_t *tsOut,
                              struct obs_source_audio_mix *audioOutput,
                              uint32_t mixers,
                              size_t channels,
                              size_t sampleRate)
{
    UNUSED_PARAMETER(sampleRate);
    obs_source_t *child = acquireChild(static_cast<NetworkDeviceSource *>(data));
    if (!child) {
        return false;
    }

    if (obs_source_audio_pending(child)) {
        obs_source_release(child);
        return false;
    }

    const uint64_t ts = obs_source_get_audio_timestamp(child);
    if (!ts) {
        obs_source_release(child);
        return false;
    }

    struct obs_source_audio_mix childAudio {};
    obs_source_get_audio_mix(child, &childAudio);

    for (size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
        if ((mixers & (1U << mix)) == 0) {
            continue;
        }
        for (size_t ch = 0; ch < channels && ch < MAX_AUDIO_CHANNELS; ++ch) {
            float *dst = audioOutput->output[mix].data[ch];
            const float *src = childAudio.output[mix].data[ch];
            if (dst && src) {
                std::memcpy(dst, src, AUDIO_OUTPUT_FRAMES * sizeof(float));
            }
        }
    }

    *tsOut = ts;
    obs_source_release(child);
    return true;
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
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                        OBS_SOURCE_COMPOSITE | OBS_SOURCE_SRGB;
    info.get_name = networkDeviceGetName;
    info.create = networkDeviceCreate;
    info.destroy = networkDeviceDestroy;
    info.get_defaults = networkDeviceDefaults;
    info.get_properties = networkDeviceProperties;
    info.update = networkDeviceUpdate;
    info.get_width = networkDeviceGetWidth;
    info.get_height = networkDeviceGetHeight;
    info.video_render = networkDeviceVideoRender;
    info.audio_render = networkDeviceAudioRender;
    info.enum_active_sources = networkDeviceEnumSources;
    info.enum_all_sources = networkDeviceEnumSources;
    info.video_get_color_space = networkDeviceGetColorSpace;
    info.icon_type = OBS_ICON_TYPE_CAMERA;

    obs_register_source(&info);
}
