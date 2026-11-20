#pragma once

#include <QString>
#include <QVector>

namespace netdev {

enum class DeviceProtocol {
    Onvif,
    // Future: Ndi, Rtsp
};

inline QString protocolDisplayName(DeviceProtocol protocol)
{
    switch (protocol) {
    case DeviceProtocol::Onvif:
        return QStringLiteral("ONVIF");
    }
    return QStringLiteral("Unknown");
}

struct MediaProfile {
    QString token;
    QString name;
    QString videoEncoding;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool hasAudio = false;
    bool hasPtz = false;
    bool isMedia2 = false;
    QString ptzToken;
    QString rtspUri;
    QString snapshotUri;
};

struct PtzPreset {
    QString token;
    QString name;
};

struct DiscoveredDevice {
    QString id;
    DeviceProtocol protocol = DeviceProtocol::Onvif;
    QString name;
    QString manufacturer;
    QString model;
    QString firmwareVersion;
    QString serialNumber;
    QString ip;
    quint16 port = 80;
    QString deviceServiceUrl;
    QString mediaServiceUrl;
    QString media2ServiceUrl;
    QString ptzServiceUrl;
    qint64 clockOffsetSecs = 0;
    bool requiresAuth = false;
    bool inspected = false;
    bool hasPtz = false;
    QVector<MediaProfile> profiles;
};

struct SourceConfig {
    DeviceProtocol protocol = DeviceProtocol::Onvif;
    QString deviceServiceUrl;
    QString mediaServiceUrl;
    QString ptzServiceUrl;
    QString deviceName;
    QString username;
    QString password;
    QString profileToken;
    QString profileName;
    QString rtspUri;
    QString snapshotUri;
    QString rtspTransport = QStringLiteral("tcp");
    bool hwDecode = false;
    bool lowLatency = true;
    bool hasPtz = false;
    bool autoReconnect = true;
    qint64 clockOffsetSecs = 0;
};

struct PtzCameraEntry {
    QString sourceUuid;
    QString sourceName;
    QString deviceName;
    QString ptzServiceUrl;
    QString profileToken;
    QString username;
    QString password;
    qint64 clockOffsetSecs = 0;
};

} // namespace netdev
