#pragma once

#include <QByteArray>
#include <QString>

namespace netdev {

class OnvifAuth {
public:
    static QString buildPasswordDigest(const QByteArray &rawNonce,
                                       const QString &createdIsoUtc,
                                       const QString &password);

    static QString buildWsSecurityHeader(const QString &username,
                                         const QString &password,
                                         qint64 clockOffsetSecs = 0);

    static QString injectRtspCredentials(const QString &rtspUri,
                                         const QString &username,
                                         const QString &password);

    static QString escapeXml(const QString &text);
};

} // namespace netdev
