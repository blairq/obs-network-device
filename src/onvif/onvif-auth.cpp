#include "onvif-auth.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QUrl>

namespace netdev {

QString OnvifAuth::escapeXml(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar &ch : text) {
        switch (ch.unicode()) {
        case '&':
            out += QStringLiteral("&amp;");
            break;
        case '<':
            out += QStringLiteral("&lt;");
            break;
        case '>':
            out += QStringLiteral("&gt;");
            break;
        case '"':
            out += QStringLiteral("&quot;");
            break;
        case '\'':
            out += QStringLiteral("&apos;");
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

QString OnvifAuth::buildPasswordDigest(const QByteArray &rawNonce,
                                       const QString &createdIsoUtc,
                                       const QString &password)
{
    QByteArray payload;
    payload.reserve(rawNonce.size() + createdIsoUtc.size() + password.size());
    payload.append(rawNonce);
    payload.append(createdIsoUtc.toUtf8());
    payload.append(password.toUtf8());

    const QByteArray sha1 = QCryptographicHash::hash(payload, QCryptographicHash::Sha1);
    return QString::fromLatin1(sha1.toBase64());
}

QString OnvifAuth::buildWsSecurityHeader(const QString &username,
                                         const QString &password,
                                         qint64 clockOffsetSecs)
{
    if (username.isEmpty()) {
        return QString();
    }

    QByteArray rawNonce(16, Qt::Uninitialized);
    auto *gen = QRandomGenerator::system();
    for (int i = 0; i < rawNonce.size(); i += 4) {
        const quint32 r = gen->generate();
        rawNonce[i] = static_cast<char>((r >> 24) & 0xFF);
        rawNonce[i + 1] = static_cast<char>((r >> 16) & 0xFF);
        rawNonce[i + 2] = static_cast<char>((r >> 8) & 0xFF);
        rawNonce[i + 3] = static_cast<char>(r & 0xFF);
    }

    const QString created = QDateTime::currentDateTimeUtc()
                                .addSecs(clockOffsetSecs)
                                .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
    const QString digest = buildPasswordDigest(rawNonce, created, password);
    const QString nonceB64 = QString::fromLatin1(rawNonce.toBase64());

    return QStringLiteral(
               "<wsse:Security soap:mustUnderstand=\"1\" "
               "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
               "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
               "<wsse:UsernameToken>"
               "<wsse:Username>%1</wsse:Username>"
               "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%2</wsse:Password>"
               "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</wsse:Nonce>"
               "<wsu:Created>%4</wsu:Created>"
               "</wsse:UsernameToken>"
               "</wsse:Security>")
        .arg(escapeXml(username), digest, nonceB64, created);
}

QString OnvifAuth::injectRtspCredentials(const QString &rtspUri,
                                         const QString &username,
                                         const QString &password)
{
    if (rtspUri.isEmpty() || username.isEmpty()) {
        return rtspUri;
    }

    QUrl url(rtspUri);
    if (!url.isValid() || url.scheme().isEmpty()) {
        return rtspUri;
    }

    if (!url.userName().isEmpty()) {
        return url.toString(QUrl::FullyEncoded);
    }

    url.setUserName(QString::fromLatin1(QUrl::toPercentEncoding(username)), QUrl::StrictMode);
    if (!password.isEmpty()) {
        url.setPassword(QString::fromLatin1(QUrl::toPercentEncoding(password)), QUrl::StrictMode);
    }
    return url.toString(QUrl::FullyEncoded);
}

} // namespace netdev
