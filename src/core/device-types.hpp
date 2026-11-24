#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>
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
    QString audioEncoding;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool hasAudio = false;
    bool hasPtz = false;
    bool isMedia2 = false;
    QString ptzToken;
    QString rtspUri;
    QString media2RtspUri;
    QString httpUri;
    QString snapshotUri;
};

struct PtzPreset {
    QString token;
    QString name;
};

struct PtzPosition {
    double pan = 0.0;
    double tilt = 0.0;
    double zoom = 0.0;
    bool hasPanTilt = false;
    bool hasZoom = false;
};

struct PtzStatus {
    PtzPosition position;
    bool panTiltMoving = false;
    bool zoomMoving = false;
    bool valid = false;
};

struct PtzLimits {
    double panMin = -1.0;
    double panMax = 1.0;
    double tiltMin = -1.0;
    double tiltMax = 1.0;
    double zoomMin = 0.0;
    double zoomMax = 1.0;
    bool hasLimits = false;
};

enum class FrameCaptureMode {
    AutoOnvifThenObs,
    OnvifSnapshotOnly,
    ObsSourceOnly,
};

struct PanoramaTileMeta {
    int col = 0;
    int row = 0;
    double pan = 0.0;
    double tilt = 0.0;
    double centerX = 0.0;
    double centerY = 0.0;
    double tileW = 0.0;
    double tileH = 0.0;
};

struct PanoramaMeta {
    QString imageFile;
    int width = 0;
    int height = 0;
    double panMin = -1.0;
    double panMax = 1.0;
    double tiltMin = -1.0;
    double tiltMax = 1.0;
    double scanZoom = 0.0;
    int cols = 0;
    int rows = 0;
    bool invertPan = false;
    bool invertTilt = false;
    QString capturedAtIso;
    QVector<PanoramaTileMeta> tiles;

    bool isValid() const { return width > 0 && height > 0 && cols > 0 && rows > 0 && !tiles.isEmpty(); }
};

struct SavedPtzPreset {
    QString id;
    QString name;
    double pan = 0.0;
    double tilt = 0.0;
    double zoom = 0.0;
    bool hasPosition = false;
    QString cameraPresetToken;
    QString thumbnailFile;
    QString createdAtIso;
};

struct CameraPtzProfile {
    QString cameraKey;
    QString cameraName;
    QString manufacturer;
    QString model;
    QString serialNumber;
    QString deviceServiceUrl;
    QString ptzServiceUrl;
    bool invertPan = false;
    bool invertTilt = false;
    double maxOpticalZoom = 12.0;
    PanoramaMeta panorama;
    QVector<SavedPtzPreset> savedPresets;
};

struct PanoramaScanSettings {
    int cols = 5;
    int rows = 3;
    double panMin = -0.85;
    double panMax = 0.85;
    double tiltMin = -0.45;
    double tiltMax = 0.45;
    double scanZoom = 0.0;
    int settleDelayMs = 900;
    int moveTimeoutMs = 3500;
    bool invertPan = false;
    bool invertTilt = false;
    FrameCaptureMode captureMode = FrameCaptureMode::AutoOnvifThenObs;
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
    QString media2ServiceUrl;
    QString ptzServiceUrl;
    QString deviceName;
    QString manufacturer;
    QString model;
    QString serialNumber;
    QString username;
    QString password;
    QString profileToken;
    QString profileName;
    QString rtspUri;
    QString snapshotUri;
    QString rtspTransport = QStringLiteral("tcp");
    QString inputFormat;
    QString latencyMode = QStringLiteral("ultra");
    int bufferingMb = 0;
    bool hwDecode = false;
    bool lowLatency = true;
    bool enableAudio = true;
    int snapshotFps = 15;
    QString extraFfmpegOptions;
    bool hasPtz = false;
    bool autoReconnect = true;
    qint64 clockOffsetSecs = 0;
    QVector<MediaProfile> detectedProfiles;
};

struct PtzCameraEntry {
    QString sourceUuid;
    QString sourceName;
    QString deviceName;
    QString manufacturer;
    QString model;
    QString serialNumber;
    QString deviceServiceUrl;
    QString ptzServiceUrl;
    QString snapshotUri;
    QString profileToken;
    QString username;
    QString password;
    qint64 clockOffsetSecs = 0;
    QString cameraKey;
    QString fallbackHostKey;
};

inline QString sanitizeCameraKeyPart(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    bool prevUnderscore = false;
    for (const QChar ch : raw.trimmed().toLower()) {
        const ushort u = ch.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_';
        if (ok) {
            out.append(ch);
            prevUnderscore = (u == '_');
        } else if (!out.isEmpty() && !prevUnderscore) {
            out.append(QLatin1Char('_'));
            prevUnderscore = true;
        }
    }
    while (out.endsWith(QLatin1Char('_'))) {
        out.chop(1);
    }
    while (out.startsWith(QLatin1Char('_'))) {
        out.remove(0, 1);
    }
    return out;
}

inline QString computeCameraFallbackHostKey(const QString &deviceServiceUrl,
                                            const QString &ptzServiceUrl,
                                            const QString &deviceName)
{
    const QString urlStr = !deviceServiceUrl.trimmed().isEmpty() ? deviceServiceUrl : ptzServiceUrl;
    const QUrl url(urlStr.trimmed());
    QStringList parts;
    if (url.isValid() && !url.host().isEmpty()) {
        const int defaultPort =
            url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
        const int port = url.port(defaultPort);
        const QString h = sanitizeCameraKeyPart(url.host());
        if (!h.isEmpty()) {
            parts << h << QString::number(port);
        }
    }
    const QString n = sanitizeCameraKeyPart(deviceName);
    if (!n.isEmpty()) {
        parts << n;
    }
    if (parts.isEmpty()) {
        return QStringLiteral("host_unknown");
    }
    return QStringLiteral("host_") + parts.join(QLatin1Char('_'));
}

inline QString computeCameraStorageKey(const QString &manufacturer,
                                       const QString &model,
                                       const QString &serialNumber,
                                       const QString &deviceServiceUrl,
                                       const QString &ptzServiceUrl,
                                       const QString &deviceName)
{
    const QString s = sanitizeCameraKeyPart(serialNumber);
    if (!s.isEmpty()) {
        QStringList parts;
        const QString mfg = sanitizeCameraKeyPart(manufacturer);
        const QString mdl = sanitizeCameraKeyPart(model);
        if (!mfg.isEmpty()) {
            parts << mfg;
        }
        if (!mdl.isEmpty()) {
            parts << mdl;
        }
        parts << s;
        return QStringLiteral("serial_") + parts.join(QLatin1Char('_'));
    }
    return computeCameraFallbackHostKey(deviceServiceUrl, ptzServiceUrl, deviceName);
}

} // namespace netdev
