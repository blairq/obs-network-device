#pragma once

#include "core/device-types.hpp"

#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

namespace netdev {

class OnvifClient : public QObject {
    Q_OBJECT

public:
    using StatusCallback = std::function<void(bool ok, const QString &error)>;
    using PresetsCallback = std::function<void(bool ok, const QVector<PtzPreset> &presets, const QString &error)>;

    explicit OnvifClient(QObject *parent = nullptr);
    ~OnvifClient() override;

    static QString normalizeDeviceServiceUrl(const QString &hostOrUrl, quint16 defaultPort = 80);

    void inspectDevice(const QString &serviceUrl, const QString &username, const QString &password);
    void fetchSnapshot(const QString &snapshotUri, const QString &username, const QString &password);

    void continuousMove(const QString &ptzUrl,
                        const QString &profileToken,
                        const QString &username,
                        const QString &password,
                        qint64 clockOffsetSecs,
                        double pan,
                        double tilt,
                        double zoom,
                        StatusCallback cb = nullptr);

    void stopPtz(const QString &ptzUrl,
                 const QString &profileToken,
                 const QString &username,
                 const QString &password,
                 qint64 clockOffsetSecs,
                 StatusCallback cb = nullptr);

    void gotoHome(const QString &ptzUrl,
                  const QString &profileToken,
                  const QString &username,
                  const QString &password,
                  qint64 clockOffsetSecs,
                  StatusCallback cb = nullptr);

    void getPresets(const QString &ptzUrl,
                    const QString &profileToken,
                    const QString &username,
                    const QString &password,
                    qint64 clockOffsetSecs,
                    PresetsCallback cb);

    void gotoPreset(const QString &ptzUrl,
                    const QString &profileToken,
                    const QString &presetToken,
                    const QString &username,
                    const QString &password,
                    qint64 clockOffsetSecs,
                    StatusCallback cb = nullptr);

    void setPreset(const QString &ptzUrl,
                   const QString &profileToken,
                   const QString &presetName,
                   const QString &username,
                   const QString &password,
                   qint64 clockOffsetSecs,
                   StatusCallback cb = nullptr);

    void removePreset(const QString &ptzUrl,
                      const QString &profileToken,
                      const QString &presetToken,
                      const QString &username,
                      const QString &password,
                      qint64 clockOffsetSecs,
                      StatusCallback cb = nullptr);

    // Pure XML parsing helpers (exposed for unit tests)
    static bool parseSystemDateAndTime(const QByteArray &xml,
                                       qint64 &offsetSecsOut,
                                       const QDateTime &localUtcOverride = QDateTime());
    static bool parseDeviceInformation(const QByteArray &xml, DiscoveredDevice &deviceOut);
    static bool parseServices(const QByteArray &xml,
                              QString &mediaUrlOut,
                              QString &media2UrlOut,
                              QString &ptzUrlOut);
    static bool parseCapabilities(const QByteArray &xml, QString &mediaUrlOut, QString &ptzUrlOut);
    static QVector<MediaProfile> parseProfilesMedia1(const QByteArray &xml);
    static QVector<MediaProfile> parseProfilesMedia2(const QByteArray &xml);
    static QString parseStreamUri(const QByteArray &xml);
    static QString parseSnapshotUri(const QByteArray &xml);
    static QVector<PtzPreset> parsePresets(const QByteArray &xml);
    static bool isAuthFault(const QByteArray &xml, int httpStatus);
    static bool isSoapVersionMismatch(const QByteArray &xml, int httpStatus);

signals:
    void deviceInspected(const netdev::DiscoveredDevice &device);
    void authRequired(const QString &serviceUrl, const QString &message);
    void inspectionFailed(const QString &serviceUrl, const QString &error);
    void snapshotReady(const QImage &image);
    void snapshotError(const QString &error);

private:
    using SoapCallback = std::function<void(int httpStatus, const QByteArray &body, const QString &netError)>;

    void sendSoapRequest(const QString &url,
                         const QString &soapAction,
                         const QString &bodyXml,
                         const QString &username,
                         const QString &password,
                         qint64 clockOffsetSecs,
                         SoapCallback cb,
                         bool useSoap11 = false);

    static QByteArray buildSoapEnvelope(const QString &securityHeaderXml,
                                        const QString &bodyXml,
                                        bool useSoap11);
    static QString rewriteHostIfInvalid(const QString &candidateUrl, const QString &referenceUrl);

    QNetworkAccessManager m_nam;
    quint64 m_inspectionGen = 0;
    quint64 m_snapshotGen = 0;
};

} // namespace netdev
