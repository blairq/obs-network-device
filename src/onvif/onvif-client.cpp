#include "onvif-client.hpp"
#include "onvif-auth.hpp"

#include <QAuthenticator>
#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSharedPointer>
#include <QTimeZone>
#include <QUrl>
#include <QXmlStreamReader>

namespace netdev {

namespace {

constexpr int kRequestTimeoutMs = 5000;

QDateTime makeUtcDateTime(int year, int month, int day, int hour, int minute, int second)
{
    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QDateTime(date, time, QTimeZone::utc());
#else
    return QDateTime(date, time, Qt::UTC);
#endif
}

QString extractFaultMessage(const QByteArray &xml, int httpStatus, const QString &netError)
{
    QXmlStreamReader reader(xml);
    QString reason;
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement) {
            const auto name = reader.name();
            if (name == QLatin1String("Text") || name == QLatin1String("faultstring")) {
                const QString t = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                if (!t.isEmpty()) {
                    reason = t;
                    break;
                }
            }
        }
    }
    if (!reason.isEmpty()) {
        return reason;
    }
    if (httpStatus > 0) {
        return QStringLiteral("HTTP %1%2").arg(httpStatus).arg(netError.isEmpty() ? QString() : QStringLiteral(" - ") + netError);
    }
    return netError.isEmpty() ? QStringLiteral("Error de red") : netError;
}

} // namespace

OnvifClient::OnvifClient(QObject *parent) : QObject(parent)
{
    connect(&m_nam, &QNetworkAccessManager::authenticationRequired, this,
            [](QNetworkReply *reply, QAuthenticator *auth) {
                if (!reply || !auth) {
                    return;
                }
                const QString user = reply->property("_onvifUser").toString();
                const QString pass = reply->property("_onvifPass").toString();
                const int tries = reply->property("_onvifAuthTries").toInt();
                if (user.isEmpty() || tries >= 1) {
                    return;
                }
                reply->setProperty("_onvifAuthTries", tries + 1);
                auth->setUser(user);
                auth->setPassword(pass);
            });
}

OnvifClient::~OnvifClient()
{
    const auto replies = m_nam.findChildren<QNetworkReply *>();
    for (QNetworkReply *reply : replies) {
        if (reply) {
            reply->disconnect(this);
            reply->abort();
        }
    }
}

QString OnvifClient::normalizeDeviceServiceUrl(const QString &hostOrUrl, quint16 defaultPort)
{
    QString trimmed = hostOrUrl.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    if (!trimmed.contains(QStringLiteral("://"))) {
        trimmed.prepend(QStringLiteral("http://"));
    }
    QUrl url(trimmed);
    if (!url.isValid() || url.host().isEmpty()) {
        return QString();
    }
    if (url.port() <= 0 && defaultPort != 80 && defaultPort != 443) {
        url.setPort(defaultPort);
    }
    if (url.path().isEmpty() || url.path() == QStringLiteral("/")) {
        url.setPath(QStringLiteral("/onvif/device_service"));
    }
    return url.toString();
}

QString OnvifClient::rewriteHostIfInvalid(const QString &candidateUrl, const QString &referenceUrl)
{
    if (candidateUrl.isEmpty()) {
        return candidateUrl;
    }
    QUrl cand(candidateUrl);
    const QUrl ref(referenceUrl);
    if (!cand.isValid() || !ref.isValid() || ref.host().isEmpty()) {
        return candidateUrl;
    }
    const QString h = cand.host();
    if (h.isEmpty() || h == QStringLiteral("0.0.0.0") || h == QStringLiteral("127.0.0.1")) {
        cand.setHost(ref.host());
    }
    return cand.toString();
}

QByteArray OnvifClient::buildSoapEnvelope(const QString &securityHeaderXml,
                                          const QString &bodyXml,
                                          bool useSoap11)
{
    const QString envelopeNs = useSoap11
                                   ? QStringLiteral("http://schemas.xmlsoap.org/soap/envelope/")
                                   : QStringLiteral("http://www.w3.org/2003/05/soap-envelope");

    QString xml;
    xml.reserve(512 + securityHeaderXml.size() + bodyXml.size());
    xml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    xml += QStringLiteral("<soap:Envelope xmlns:soap=\"%1\" "
                          "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
                          "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
                          "xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\" "
                          "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" "
                          "xmlns:tt=\"http://www.onvif.org/ver10/schema\">")
               .arg(envelopeNs);
    if (!securityHeaderXml.isEmpty()) {
        xml += QStringLiteral("<soap:Header>") + securityHeaderXml + QStringLiteral("</soap:Header>");
    }
    xml += QStringLiteral("<soap:Body>") + bodyXml + QStringLiteral("</soap:Body>");
    xml += QStringLiteral("</soap:Envelope>");
    return xml.toUtf8();
}

void OnvifClient::sendSoapRequest(const QString &url,
                                  const QString &soapAction,
                                  const QString &bodyXml,
                                  const QString &username,
                                  const QString &password,
                                  qint64 clockOffsetSecs,
                                  SoapCallback cb,
                                  bool useSoap11)
{
    const QUrl targetUrl(url);
    if (!targetUrl.isValid()) {
        if (cb) {
            cb(0, QByteArray(), QStringLiteral("URL invalida: %1").arg(url));
        }
        return;
    }

    QNetworkRequest req(targetUrl);
    req.setTransferTimeout(kRequestTimeoutMs);
    if (useSoap11) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=utf-8"));
        if (!soapAction.isEmpty()) {
            req.setRawHeader("SOAPAction", QStringLiteral("\"%1\"").arg(soapAction).toUtf8());
        }
    } else {
        QString ct = QStringLiteral("application/soap+xml; charset=utf-8");
        if (!soapAction.isEmpty()) {
            ct += QStringLiteral("; action=\"%1\"").arg(soapAction);
        }
        req.setHeader(QNetworkRequest::ContentTypeHeader, ct);
    }

    const QString secHeader = OnvifAuth::buildWsSecurityHeader(username, password, clockOffsetSecs);
    const QByteArray payload = buildSoapEnvelope(secHeader, bodyXml, useSoap11);

    QNetworkReply *reply = m_nam.post(req, payload);
    reply->setProperty("_onvifUser", username);
    reply->setProperty("_onvifPass", password);
    reply->setProperty("_onvifAuthTries", 0);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, soapAction, bodyXml, username, password, clockOffsetSecs, cb, useSoap11]() {
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();
                const QString netErr = reply->error() != QNetworkReply::NoError ? reply->errorString() : QString();
                reply->deleteLater();

                if (!useSoap11 && isSoapVersionMismatch(body, status)) {
                    sendSoapRequest(url, soapAction, bodyXml, username, password, clockOffsetSecs, cb, true);
                    return;
                }

                if (cb) {
                    cb(status, body, netErr);
                }
            });
}

void OnvifClient::inspectDevice(const QString &serviceUrl, const QString &username, const QString &password)
{
    const QString normalizedUrl = normalizeDeviceServiceUrl(serviceUrl);
    if (normalizedUrl.isEmpty()) {
        emit inspectionFailed(serviceUrl, QStringLiteral("URL de servicio ONVIF invalida."));
        return;
    }

    const quint64 gen = ++m_inspectionGen;

    struct InspectState {
        DiscoveredDevice dev;
        QString username;
        QString password;
        qsizetype pendingProfileRequests = 0;
        bool failed = false;
    };

    auto state = QSharedPointer<InspectState>::create();
    state->dev.protocol = DeviceProtocol::Onvif;
    state->dev.deviceServiceUrl = normalizedUrl;
    state->dev.id = normalizedUrl;
    const QUrl parsedUrl(normalizedUrl);
    state->dev.ip = parsedUrl.host();
    state->dev.port = static_cast<quint16>(
        parsedUrl.port(parsedUrl.scheme() == QStringLiteral("https") ? 443 : 80));
    state->username = username;
    state->password = password;

    auto finishIfDone = [this, gen, state]() {
        if (gen != m_inspectionGen || state->failed) {
            return;
        }
        if (state->pendingProfileRequests > 0) {
            return;
        }
        state->dev.inspected = true;
        if (state->dev.name.isEmpty()) {
            const QString combined = QStringLiteral("%1 %2")
                                         .arg(state->dev.manufacturer, state->dev.model)
                                         .trimmed();
            state->dev.name = combined.isEmpty()
                                  ? QStringLiteral("ONVIF (%1)").arg(state->dev.ip)
                                  : combined;
        }
        emit deviceInspected(state->dev);
    };

    auto fetchProfileUris = [this, gen, state, finishIfDone]() {
        if (gen != m_inspectionGen || state->failed) {
            return;
        }
        if (state->dev.profiles.isEmpty()) {
            finishIfDone();
            return;
        }

        state->pendingProfileRequests = state->dev.profiles.size() * 2;

        for (qsizetype idx = 0; idx < state->dev.profiles.size(); ++idx) {
            const MediaProfile &prof = state->dev.profiles.at(idx);
            const QString mediaEndpoint = prof.isMedia2 && !state->dev.media2ServiceUrl.isEmpty()
                                              ? state->dev.media2ServiceUrl
                                              : state->dev.mediaServiceUrl;

            // 1) GetStreamUri
            const QString streamBody = prof.isMedia2
                                           ? QStringLiteral(
                                                 "<tr2:GetStreamUri>"
                                                 "<tr2:Protocol>RTSP</tr2:Protocol>"
                                                 "<tr2:ProfileToken>%1</tr2:ProfileToken>"
                                                 "</tr2:GetStreamUri>")
                                                 .arg(OnvifAuth::escapeXml(prof.token))
                                           : QStringLiteral(
                                                 "<trt:GetStreamUri>"
                                                 "<trt:StreamSetup>"
                                                 "<tt:Stream>RTP-Unicast</tt:Stream>"
                                                 "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
                                                 "</trt:StreamSetup>"
                                                 "<trt:ProfileToken>%1</trt:ProfileToken>"
                                                 "</trt:GetStreamUri>")
                                                 .arg(OnvifAuth::escapeXml(prof.token));

            const QString streamAction = prof.isMedia2
                                             ? QStringLiteral("http://www.onvif.org/ver20/media/wsdl/GetStreamUri")
                                             : QStringLiteral("http://www.onvif.org/ver10/media/wsdl/GetStreamUri");

            sendSoapRequest(
                mediaEndpoint, streamAction, streamBody, state->username, state->password,
                state->dev.clockOffsetSecs,
                [this, gen, state, idx, finishIfDone](int status, const QByteArray &body, const QString &) {
                    if (gen != m_inspectionGen || state->failed) {
                        return;
                    }
                    if (isAuthFault(body, status)) {
                        state->failed = true;
                        state->dev.requiresAuth = true;
                        emit authRequired(state->dev.deviceServiceUrl,
                                          QStringLiteral("La camara requiere usuario y contraseña validos."));
                        return;
                    }
                    const QString uri = rewriteHostIfInvalid(parseStreamUri(body),
                                                             state->dev.deviceServiceUrl);
                    if (!uri.isEmpty() && idx < state->dev.profiles.size()) {
                        state->dev.profiles[idx].rtspUri = uri;
                    }
                    state->pendingProfileRequests--;
                    finishIfDone();
                });

            // 2) GetSnapshotUri
            const QString snapBody = prof.isMedia2
                                         ? QStringLiteral("<tr2:GetSnapshotUri><tr2:ProfileToken>%1</tr2:ProfileToken></tr2:GetSnapshotUri>")
                                               .arg(OnvifAuth::escapeXml(prof.token))
                                         : QStringLiteral("<trt:GetSnapshotUri><trt:ProfileToken>%1</trt:ProfileToken></trt:GetSnapshotUri>")
                                               .arg(OnvifAuth::escapeXml(prof.token));

            const QString snapAction = prof.isMedia2
                                           ? QStringLiteral("http://www.onvif.org/ver20/media/wsdl/GetSnapshotUri")
                                           : QStringLiteral("http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri");

            sendSoapRequest(
                mediaEndpoint, snapAction, snapBody, state->username, state->password,
                state->dev.clockOffsetSecs,
                [this, gen, state, idx, finishIfDone](int, const QByteArray &body, const QString &) {
                    if (gen != m_inspectionGen || state->failed) {
                        return;
                    }
                    const QString uri = rewriteHostIfInvalid(parseSnapshotUri(body),
                                                             state->dev.deviceServiceUrl);
                    if (!uri.isEmpty() && idx < state->dev.profiles.size()) {
                        state->dev.profiles[idx].snapshotUri = uri;
                    }
                    state->pendingProfileRequests--;
                    finishIfDone();
                });
        }
    };

    auto fetchProfiles = [this, gen, state, fetchProfileUris]() {
        if (gen != m_inspectionGen || state->failed) {
            return;
        }
        if (state->dev.mediaServiceUrl.isEmpty() && state->dev.media2ServiceUrl.isEmpty()) {
            emit inspectionFailed(state->dev.deviceServiceUrl,
                                  QStringLiteral("El dispositivo no expone servicio ONVIF Media."));
            state->failed = true;
            return;
        }

        auto queryMedia2 = [this, gen, state, fetchProfileUris]() {
            if (state->dev.media2ServiceUrl.isEmpty()) {
                fetchProfileUris();
                return;
            }
            const QString bodyXml = QStringLiteral("<tr2:GetProfiles><tr2:Type>All</tr2:Type></tr2:GetProfiles>");
            sendSoapRequest(
                state->dev.media2ServiceUrl,
                QStringLiteral("http://www.onvif.org/ver20/media/wsdl/GetProfiles"),
                bodyXml, state->username, state->password, state->dev.clockOffsetSecs,
                [this, gen, state, fetchProfileUris](int status, const QByteArray &body, const QString &err) {
                    if (gen != m_inspectionGen || state->failed) {
                        return;
                    }
                    if (isAuthFault(body, status)) {
                        state->failed = true;
                        emit authRequired(state->dev.deviceServiceUrl,
                                          QStringLiteral("Autenticacion requerida para leer perfiles Media2."));
                        return;
                    }
                    const auto m2Profiles = parseProfilesMedia2(body);
                    for (const MediaProfile &p2 : m2Profiles) {
                        bool exists = false;
                        for (MediaProfile &p1 : state->dev.profiles) {
                            if (p1.token == p2.token) {
                                exists = true;
                                if (p1.videoEncoding.isEmpty() && !p2.videoEncoding.isEmpty()) {
                                    p1.videoEncoding = p2.videoEncoding;
                                    p1.isMedia2 = true;
                                }
                                if (p2.hasPtz) {
                                    p1.hasPtz = true;
                                }
                                break;
                            }
                        }
                        if (!exists) {
                            state->dev.profiles.push_back(p2);
                        }
                        if (p2.hasPtz) {
                            state->dev.hasPtz = true;
                        }
                    }
                    if (state->dev.profiles.isEmpty() && !err.isEmpty()) {
                        emit inspectionFailed(state->dev.deviceServiceUrl,
                                              extractFaultMessage(body, status, err));
                        state->failed = true;
                        return;
                    }
                    fetchProfileUris();
                });
        };

        if (!state->dev.mediaServiceUrl.isEmpty()) {
            sendSoapRequest(
                state->dev.mediaServiceUrl,
                QStringLiteral("http://www.onvif.org/ver10/media/wsdl/GetProfiles"),
                QStringLiteral("<trt:GetProfiles/>"),
                state->username, state->password, state->dev.clockOffsetSecs,
                [this, gen, state, queryMedia2](int status, const QByteArray &body, const QString &) {
                    if (gen != m_inspectionGen || state->failed) {
                        return;
                    }
                    if (isAuthFault(body, status)) {
                        state->failed = true;
                        emit authRequired(state->dev.deviceServiceUrl,
                                          QStringLiteral("La camara exige credenciales para obtener perfiles."));
                        return;
                    }
                    const auto m1Profiles = parseProfilesMedia1(body);
                    for (const MediaProfile &p : m1Profiles) {
                        state->dev.profiles.push_back(p);
                        if (p.hasPtz) {
                            state->dev.hasPtz = true;
                        }
                    }
                    queryMedia2();
                });
        } else {
            queryMedia2();
        }
    };

    auto fetchCapabilitiesFallback = [this, gen, state, fetchProfiles]() {
        const QString capBody = QStringLiteral("<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>");
        sendSoapRequest(
            state->dev.deviceServiceUrl,
            QStringLiteral("http://www.onvif.org/ver10/device/wsdl/GetCapabilities"),
            capBody, state->username, state->password, state->dev.clockOffsetSecs,
            [this, gen, state, fetchProfiles](int status, const QByteArray &body, const QString &err) {
                if (gen != m_inspectionGen || state->failed) {
                    return;
                }
                if (isAuthFault(body, status)) {
                    state->failed = true;
                    emit authRequired(state->dev.deviceServiceUrl,
                                      QStringLiteral("Credenciales requeridas por el dispositivo."));
                    return;
                }
                QString mediaUrl, ptzUrl;
                if (parseCapabilities(body, mediaUrl, ptzUrl)) {
                    if (!mediaUrl.isEmpty()) {
                        state->dev.mediaServiceUrl = rewriteHostIfInvalid(mediaUrl, state->dev.deviceServiceUrl);
                    }
                    if (!ptzUrl.isEmpty()) {
                        state->dev.ptzServiceUrl = rewriteHostIfInvalid(ptzUrl, state->dev.deviceServiceUrl);
                        state->dev.hasPtz = true;
                    }
                } else if (state->dev.mediaServiceUrl.isEmpty()) {
                    state->failed = true;
                    emit inspectionFailed(state->dev.deviceServiceUrl, extractFaultMessage(body, status, err));
                    return;
                }
                fetchProfiles();
            });
    };

    auto fetchServices = [this, gen, state, fetchProfiles, fetchCapabilitiesFallback]() {
        const QString svcBody = QStringLiteral("<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>");
        sendSoapRequest(
            state->dev.deviceServiceUrl,
            QStringLiteral("http://www.onvif.org/ver10/device/wsdl/GetServices"),
            svcBody, state->username, state->password, state->dev.clockOffsetSecs,
            [this, gen, state, fetchProfiles, fetchCapabilitiesFallback](int status, const QByteArray &body, const QString &) {
                if (gen != m_inspectionGen || state->failed) {
                    return;
                }
                if (isAuthFault(body, status)) {
                    state->failed = true;
                    emit authRequired(state->dev.deviceServiceUrl,
                                      QStringLiteral("Credenciales requeridas para consultar servicios."));
                    return;
                }
                QString media1, media2, ptz;
                if (parseServices(body, media1, media2, ptz) && (!media1.isEmpty() || !media2.isEmpty())) {
                    state->dev.mediaServiceUrl = rewriteHostIfInvalid(media1, state->dev.deviceServiceUrl);
                    state->dev.media2ServiceUrl = rewriteHostIfInvalid(media2, state->dev.deviceServiceUrl);
                    state->dev.ptzServiceUrl = rewriteHostIfInvalid(ptz, state->dev.deviceServiceUrl);
                    if (!state->dev.ptzServiceUrl.isEmpty()) {
                        state->dev.hasPtz = true;
                    }
                    fetchProfiles();
                } else {
                    fetchCapabilitiesFallback();
                }
            });
    };

    auto fetchDeviceInfo = [this, gen, state, fetchServices]() {
        sendSoapRequest(
            state->dev.deviceServiceUrl,
            QStringLiteral("http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation"),
            QStringLiteral("<tds:GetDeviceInformation/>"),
            state->username, state->password, state->dev.clockOffsetSecs,
            [this, gen, state, fetchServices](int status, const QByteArray &body, const QString &err) {
                if (gen != m_inspectionGen || state->failed) {
                    return;
                }
                if (isAuthFault(body, status)) {
                    state->failed = true;
                    state->dev.requiresAuth = true;
                    emit authRequired(state->dev.deviceServiceUrl,
                                      QStringLiteral("El dispositivo requiere usuario y contraseña."));
                    return;
                }
                if (status >= 200 && status < 300) {
                    parseDeviceInformation(body, state->dev);
                } else if (status == 0 && !err.isEmpty()) {
                    state->failed = true;
                    emit inspectionFailed(state->dev.deviceServiceUrl, err);
                    return;
                }
                fetchServices();
            });
    };

    // Step 1: Sync camera clock before any authenticated WS-UsernameToken call
    sendSoapRequest(
        state->dev.deviceServiceUrl,
        QStringLiteral("http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime"),
        QStringLiteral("<tds:GetSystemDateAndTime/>"),
        QString(), QString(), 0,
        [this, gen, state, fetchDeviceInfo](int status, const QByteArray &body, const QString &err) {
            if (gen != m_inspectionGen || state->failed) {
                return;
            }
            if (status == 0 && !err.isEmpty()) {
                state->failed = true;
                emit inspectionFailed(state->dev.deviceServiceUrl,
                                      QStringLiteral("No se pudo conectar con %1: %2")
                                          .arg(state->dev.deviceServiceUrl, err));
                return;
            }
            qint64 offset = 0;
            if (parseSystemDateAndTime(body, offset)) {
                state->dev.clockOffsetSecs = offset;
            }
            fetchDeviceInfo();
        });
}

void OnvifClient::fetchSnapshot(const QString &snapshotUri, const QString &username, const QString &password)
{
    const QUrl url(snapshotUri);
    if (!url.isValid() || url.isEmpty()) {
        emit snapshotError(QStringLiteral("La camara no reporto URL de captura."));
        return;
    }

    const quint64 gen = ++m_snapshotGen;
    QNetworkRequest req(url);
    req.setTransferTimeout(kRequestTimeoutMs);

    QNetworkReply *reply = m_nam.get(req);
    reply->setProperty("_onvifUser", username);
    reply->setProperty("_onvifPass", password);
    reply->setProperty("_onvifAuthTries", 0);

    connect(reply, &QNetworkReply::finished, this, [this, gen, reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bytes = reply->isOpen() ? reply->readAll() : QByteArray();
        const QString err = reply->error() != QNetworkReply::NoError ? reply->errorString() : QString();
        reply->deleteLater();

        if (gen != m_snapshotGen) {
            return;
        }
        if (status == 401 || status == 403) {
            emit snapshotError(QStringLiteral("Captura denegada: verifica usuario/contraseña."));
            return;
        }
        if (!err.isEmpty() || bytes.isEmpty()) {
            emit snapshotError(err.isEmpty() ? QStringLiteral("Respuesta vacia al pedir captura.") : err);
            return;
        }

        QImage img;
        if (!img.loadFromData(bytes)) {
            emit snapshotError(QStringLiteral("El dispositivo no devolvio una imagen valida."));
            return;
        }
        emit snapshotReady(img);
    });
}

void OnvifClient::continuousMove(const QString &ptzUrl,
                                 const QString &profileToken,
                                 const QString &username,
                                 const QString &password,
                                 qint64 clockOffsetSecs,
                                 double pan,
                                 double tilt,
                                 double zoom,
                                 StatusCallback cb)
{
    const QString body = QStringLiteral(
                             "<tptz:ContinuousMove>"
                             "<tptz:ProfileToken>%1</tptz:ProfileToken>"
                             "<tptz:Velocity>"
                             "<tt:PanTilt x=\"%2\" y=\"%3\"/>"
                             "<tt:Zoom x=\"%4\"/>"
                             "</tptz:Velocity>"
                             "</tptz:ContinuousMove>")
                             .arg(OnvifAuth::escapeXml(profileToken))
                             .arg(pan, 0, 'f', 2)
                             .arg(tilt, 0, 'f', 2)
                             .arg(zoom, 0, 'f', 2);

    sendSoapRequest(
        ptzUrl,
        QStringLiteral("http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove"),
        body, username, password, clockOffsetSecs,
        [cb](int status, const QByteArray &resp, const QString &err) {
            if (cb) {
                const bool ok = status >= 200 && status < 300 && err.isEmpty();
                cb(ok, ok ? QString() : extractFaultMessage(resp, status, err));
            }
        });
}

void OnvifClient::stopPtz(const QString &ptzUrl,
                          const QString &profileToken,
                          const QString &username,
                          const QString &password,
                          qint64 clockOffsetSecs,
                          StatusCallback cb)
{
    const QString body = QStringLiteral(
                             "<tptz:Stop>"
                             "<tptz:ProfileToken>%1</tptz:ProfileToken>"
                             "<tptz:PanTilt>true</tptz:PanTilt>"
                             "<tptz:Zoom>true</tptz:Zoom>"
                             "</tptz:Stop>")
                             .arg(OnvifAuth::escapeXml(profileToken));

    sendSoapRequest(
        ptzUrl,
        QStringLiteral("http://www.onvif.org/ver20/ptz/wsdl/Stop"),
        body, username, password, clockOffsetSecs,
        [cb](int status, const QByteArray &resp, const QString &err) {
            if (cb) {
                const bool ok = status >= 200 && status < 300 && err.isEmpty();
                cb(ok, ok ? QString() : extractFaultMessage(resp, status, err));
            }
        });
}

void OnvifClient::gotoHome(const QString &ptzUrl,
                           const QString &profileToken,
                           const QString &username,
                           const QString &password,
                           qint64 clockOffsetSecs,
                           StatusCallback cb)
{
    const QString body = QStringLiteral(
                             "<tptz:GotoHomePosition>"
                             "<tptz:ProfileToken>%1</tptz:ProfileToken>"
                             "</tptz:GotoHomePosition>")
                             .arg(OnvifAuth::escapeXml(profileToken));

    sendSoapRequest(
        ptzUrl,
        QStringLiteral("http://www.onvif.org/ver20/ptz/wsdl/GotoHomePosition"),
        body, username, password, clockOffsetSecs,
        [cb](int status, const QByteArray &resp, const QString &err) {
            if (cb) {
                const bool ok = status >= 200 && status < 300 && err.isEmpty();
                cb(ok, ok ? QString() : extractFaultMessage(resp, status, err));
            }
        });
}

void OnvifClient::getPresets(const QString &ptzUrl,
                             const QString &profileToken,
                             const QString &username,
                             const QString &password,
                             qint64 clockOffsetSecs,
                             PresetsCallback cb)
{
    const QString body = QStringLiteral(
                             "<tptz:GetPresets>"
                             "<tptz:ProfileToken>%1</tptz:ProfileToken>"
                             "</tptz:GetPresets>")
                             .arg(OnvifAuth::escapeXml(profileToken));

    sendSoapRequest(
        ptzUrl,
        QStringLiteral("http://www.onvif.org/ver20/ptz/wsdl/GetPresets"),
        body, username, password, clockOffsetSecs,
        [cb](int status, const QByteArray &resp, const QString &err) {
            if (!cb) {
                return;
            }
            const bool ok = status >= 200 && status < 300 && err.isEmpty();
            if (!ok) {
                cb(false, {}, extractFaultMessage(resp, status, err));
                return;
            }
            cb(true, parsePresets(resp), QString());
        });
}

void OnvifClient::gotoPreset(const QString &ptzUrl,
                             const QString &profileToken,
                             const QString &presetToken,
                             const QString &username,
                             const QString &password,
                             qint64 clockOffsetSecs,
                             StatusCallback cb)
{
    const QString body = QStringLiteral(
                             "<tptz:GotoPreset>"
                             "<tptz:ProfileToken>%1</tptz:ProfileToken>"
                             "<tptz:PresetToken>%2</tptz:PresetToken>"
                             "</tptz:GotoPreset>")
                             .arg(OnvifAuth::escapeXml(profileToken), OnvifAuth::escapeXml(presetToken));

    sendSoapRequest(
        ptzUrl,
        QStringLiteral("http://www.onvif.org/ver20/ptz/wsdl/GotoPreset"),
        body, username, password, clockOffsetSecs,
        [cb](int status, const QByteArray &resp, const QString &err) {
            if (cb) {
                const bool ok = status >= 200 && status < 300 && err.isEmpty();
                cb(ok, ok ? QString() : extractFaultMessage(resp, status, err));
            }
        });
}

void OnvifClient::setPreset(const QString &ptzUrl,
                            const QString &profileToken,
                            const QString &presetName,
                            const QString &username,
                            const QString &password,
                            qint64 clockOffsetSecs,
                            StatusCallback cb)
{
    const QString body = QStringLiteral(
                             "<tptz:SetPreset>"
                             "<tptz:ProfileToken>%1</tptz:ProfileToken>"
                             "<tptz:PresetName>%2</tptz:PresetName>"
                             "</tptz:SetPreset>")
                             .arg(OnvifAuth::escapeXml(profileToken), OnvifAuth::escapeXml(presetName));

    sendSoapRequest(
        ptzUrl,
        QStringLiteral("http://www.onvif.org/ver20/ptz/wsdl/SetPreset"),
        body, username, password, clockOffsetSecs,
        [cb](int status, const QByteArray &resp, const QString &err) {
            if (cb) {
                const bool ok = status >= 200 && status < 300 && err.isEmpty();
                cb(ok, ok ? QString() : extractFaultMessage(resp, status, err));
            }
        });
}

void OnvifClient::removePreset(const QString &ptzUrl,
                               const QString &profileToken,
                               const QString &presetToken,
                               const QString &username,
                               const QString &password,
                               qint64 clockOffsetSecs,
                               StatusCallback cb)
{
    const QString body = QStringLiteral(
                             "<tptz:RemovePreset>"
                             "<tptz:ProfileToken>%1</tptz:ProfileToken>"
                             "<tptz:PresetToken>%2</tptz:PresetToken>"
                             "</tptz:RemovePreset>")
                             .arg(OnvifAuth::escapeXml(profileToken), OnvifAuth::escapeXml(presetToken));

    sendSoapRequest(
        ptzUrl,
        QStringLiteral("http://www.onvif.org/ver20/ptz/wsdl/RemovePreset"),
        body, username, password, clockOffsetSecs,
        [cb](int status, const QByteArray &resp, const QString &err) {
            if (cb) {
                const bool ok = status >= 200 && status < 300 && err.isEmpty();
                cb(ok, ok ? QString() : extractFaultMessage(resp, status, err));
            }
        });
}

// --- Pure XML Parsers ---

bool OnvifClient::parseSystemDateAndTime(const QByteArray &xml,
                                         qint64 &offsetSecsOut,
                                         const QDateTime &localUtcOverride)
{
    QXmlStreamReader reader(xml);
    bool inUtc = false;
    bool inLocal = false;
    int utcYear = 0, utcMonth = 0, utcDay = 0, utcHour = 0, utcMin = 0, utcSec = 0;
    int locYear = 0, locMonth = 0, locDay = 0, locHour = 0, locMin = 0, locSec = 0;
    bool hasUtc = false;
    bool hasLocal = false;

    while (!reader.atEnd()) {
        const auto tok = reader.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const auto name = reader.name();
            if (name == QLatin1String("UTCDateTime")) {
                inUtc = true;
                hasUtc = true;
            } else if (name == QLatin1String("LocalDateTime")) {
                inLocal = true;
                hasLocal = true;
            } else if (inUtc || inLocal) {
                if (name == QLatin1String("Year") || name == QLatin1String("Month") ||
                    name == QLatin1String("Day") || name == QLatin1String("Hour") ||
                    name == QLatin1String("Minute") || name == QLatin1String("Second")) {
                    const int val = reader.readElementText(QXmlStreamReader::SkipChildElements).toInt();
                    if (inUtc) {
                        if (name == QLatin1String("Year")) utcYear = val;
                        else if (name == QLatin1String("Month")) utcMonth = val;
                        else if (name == QLatin1String("Day")) utcDay = val;
                        else if (name == QLatin1String("Hour")) utcHour = val;
                        else if (name == QLatin1String("Minute")) utcMin = val;
                        else if (name == QLatin1String("Second")) utcSec = val;
                    } else {
                        if (name == QLatin1String("Year")) locYear = val;
                        else if (name == QLatin1String("Month")) locMonth = val;
                        else if (name == QLatin1String("Day")) locDay = val;
                        else if (name == QLatin1String("Hour")) locHour = val;
                        else if (name == QLatin1String("Minute")) locMin = val;
                        else if (name == QLatin1String("Second")) locSec = val;
                    }
                }
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            const auto name = reader.name();
            if (name == QLatin1String("UTCDateTime")) inUtc = false;
            else if (name == QLatin1String("LocalDateTime")) inLocal = false;
        }
    }

    QDateTime camDt;
    if (hasUtc && utcYear > 1970) {
        camDt = makeUtcDateTime(utcYear, utcMonth, utcDay, utcHour, utcMin, utcSec);
    } else if (hasLocal && locYear > 1970) {
        camDt = makeUtcDateTime(locYear, locMonth, locDay, locHour, locMin, locSec);
    }

    if (!camDt.isValid()) {
        return false;
    }

    const QDateTime nowUtc = localUtcOverride.isValid() ? localUtcOverride.toUTC() : QDateTime::currentDateTimeUtc();
    offsetSecsOut = nowUtc.secsTo(camDt);
    return true;
}

bool OnvifClient::parseDeviceInformation(const QByteArray &xml, DiscoveredDevice &deviceOut)
{
    QXmlStreamReader reader(xml);
    bool foundAny = false;
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement) {
            const auto name = reader.name();
            if (name == QLatin1String("Manufacturer")) {
                deviceOut.manufacturer = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                foundAny = true;
            } else if (name == QLatin1String("Model")) {
                deviceOut.model = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                foundAny = true;
            } else if (name == QLatin1String("FirmwareVersion")) {
                deviceOut.firmwareVersion = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                foundAny = true;
            } else if (name == QLatin1String("SerialNumber")) {
                deviceOut.serialNumber = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                foundAny = true;
            }
        }
    }
    if (foundAny && (deviceOut.name.isEmpty() || deviceOut.name.startsWith(QStringLiteral("ONVIF (")))) {
        const QString combined = QStringLiteral("%1 %2").arg(deviceOut.manufacturer, deviceOut.model).trimmed();
        if (!combined.isEmpty()) {
            deviceOut.name = combined;
        }
    }
    return foundAny;
}

bool OnvifClient::parseServices(const QByteArray &xml,
                                QString &mediaUrlOut,
                                QString &media2UrlOut,
                                QString &ptzUrlOut)
{
    QXmlStreamReader reader(xml);
    bool inService = false;
    QString currentNs;
    QString currentXAddr;
    bool found = false;

    while (!reader.atEnd()) {
        const auto tok = reader.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const auto name = reader.name();
            if (name == QLatin1String("Service")) {
                inService = true;
                currentNs.clear();
                currentXAddr.clear();
            } else if (inService) {
                if (name == QLatin1String("Namespace")) {
                    currentNs = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                } else if (name == QLatin1String("XAddr")) {
                    currentXAddr = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                }
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            if (reader.name() == QLatin1String("Service") && inService) {
                if (currentNs.contains(QStringLiteral("/ver10/media/"), Qt::CaseInsensitive)) {
                    mediaUrlOut = currentXAddr;
                    found = true;
                } else if (currentNs.contains(QStringLiteral("/ver20/media/"), Qt::CaseInsensitive)) {
                    media2UrlOut = currentXAddr;
                    found = true;
                } else if (currentNs.contains(QStringLiteral("/ver20/ptz/"), Qt::CaseInsensitive)) {
                    ptzUrlOut = currentXAddr;
                    found = true;
                }
                inService = false;
            }
        }
    }
    return found;
}

bool OnvifClient::parseCapabilities(const QByteArray &xml, QString &mediaUrlOut, QString &ptzUrlOut)
{
    QXmlStreamReader reader(xml);
    bool inMedia = false;
    bool inPtz = false;
    bool found = false;

    while (!reader.atEnd()) {
        const auto tok = reader.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const auto name = reader.name();
            if (name == QLatin1String("Media")) {
                inMedia = true;
            } else if (name == QLatin1String("PTZ")) {
                inPtz = true;
            } else if (name == QLatin1String("XAddr")) {
                const QString xaddr = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                if (inMedia && mediaUrlOut.isEmpty()) {
                    mediaUrlOut = xaddr;
                    found = true;
                } else if (inPtz && ptzUrlOut.isEmpty()) {
                    ptzUrlOut = xaddr;
                    found = true;
                }
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            const auto name = reader.name();
            if (name == QLatin1String("Media")) inMedia = false;
            else if (name == QLatin1String("PTZ")) inPtz = false;
        }
    }
    return found;
}

namespace {

QVector<MediaProfile> parseProfilesImpl(const QByteArray &xml, bool isMedia2)
{
    const QLatin1String videoTag = isMedia2 ? QLatin1String("VideoEncoder")
                                            : QLatin1String("VideoEncoderConfiguration");
    const QLatin1String audioEncTag = isMedia2 ? QLatin1String("AudioEncoder")
                                               : QLatin1String("AudioEncoderConfiguration");
    const QLatin1String audioSrcTag = isMedia2 ? QLatin1String("AudioSource")
                                               : QLatin1String("AudioSourceConfiguration");
    const QLatin1String ptzTag = isMedia2 ? QLatin1String("PTZ")
                                          : QLatin1String("PTZConfiguration");

    QVector<MediaProfile> profiles;
    QXmlStreamReader reader(xml);

    bool inProfile = false;
    int profileDepth = 0;
    int depth = 0;
    bool inVideoEnc = false;
    bool inPtz = false;
    MediaProfile cur;

    while (!reader.atEnd()) {
        const auto tok = reader.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            ++depth;
            const auto name = reader.name();
            if (name == QLatin1String("Profiles")) {
                inProfile = true;
                profileDepth = depth;
                cur = MediaProfile{};
                cur.isMedia2 = isMedia2;
                cur.token = reader.attributes().value(QStringLiteral("token")).toString().trimmed();
            } else if (inProfile) {
                if (name == QLatin1String("Name") && depth == profileDepth + 1 && cur.name.isEmpty()) {
                    cur.name = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                    --depth;
                } else if (name == videoTag) {
                    inVideoEnc = true;
                } else if (name == audioEncTag || name == audioSrcTag) {
                    cur.hasAudio = true;
                } else if (name == ptzTag) {
                    inPtz = true;
                    cur.hasPtz = true;
                } else if (inVideoEnc) {
                    if (name == QLatin1String("Encoding")) {
                        cur.videoEncoding = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                        --depth;
                    } else if (name == QLatin1String("Width")) {
                        cur.width = reader.readElementText(QXmlStreamReader::SkipChildElements).toInt();
                        --depth;
                    } else if (name == QLatin1String("Height")) {
                        cur.height = reader.readElementText(QXmlStreamReader::SkipChildElements).toInt();
                        --depth;
                    } else if (name == QLatin1String("FrameRateLimit")) {
                        cur.fps = reader.readElementText(QXmlStreamReader::SkipChildElements).toDouble();
                        --depth;
                    }
                } else if (inPtz && name == QLatin1String("NodeToken")) {
                    cur.ptzToken = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                    --depth;
                }
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            const auto name = reader.name();
            if (name == videoTag) {
                inVideoEnc = false;
            } else if (name == ptzTag) {
                inPtz = false;
            } else if (name == QLatin1String("Profiles") && inProfile && depth == profileDepth) {
                if (!cur.token.isEmpty()) {
                    if (cur.name.isEmpty()) {
                        cur.name = cur.token;
                    }
                    profiles.push_back(cur);
                }
                inProfile = false;
            }
            --depth;
        }
    }
    return profiles;
}

} // namespace

QVector<MediaProfile> OnvifClient::parseProfilesMedia1(const QByteArray &xml)
{
    return parseProfilesImpl(xml, false);
}

QVector<MediaProfile> OnvifClient::parseProfilesMedia2(const QByteArray &xml)
{
    return parseProfilesImpl(xml, true);
}

QString OnvifClient::parseStreamUri(const QByteArray &xml)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement) {
            if (reader.name() == QLatin1String("Uri")) {
                return reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            }
        }
    }
    return QString();
}

QString OnvifClient::parseSnapshotUri(const QByteArray &xml)
{
    return parseStreamUri(xml);
}

QVector<PtzPreset> OnvifClient::parsePresets(const QByteArray &xml)
{
    QVector<PtzPreset> presets;
    QXmlStreamReader reader(xml);
    bool inPreset = false;
    PtzPreset cur;

    while (!reader.atEnd()) {
        const auto tok = reader.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const auto name = reader.name();
            if (name == QLatin1String("Preset")) {
                inPreset = true;
                cur = PtzPreset{};
                cur.token = reader.attributes().value(QStringLiteral("token")).toString().trimmed();
            } else if (inPreset && name == QLatin1String("Name")) {
                cur.name = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            if (reader.name() == QLatin1String("Preset") && inPreset) {
                if (!cur.token.isEmpty()) {
                    if (cur.name.isEmpty()) cur.name = cur.token;
                    presets.push_back(cur);
                }
                inPreset = false;
            }
        }
    }
    return presets;
}

bool OnvifClient::isAuthFault(const QByteArray &xml, int httpStatus)
{
    if (httpStatus == 401 || httpStatus == 403) {
        return true;
    }
    const QString text = QString::fromUtf8(xml);
    if (!text.contains(QStringLiteral("Fault"), Qt::CaseInsensitive)) {
        return false;
    }
    return text.contains(QStringLiteral("NotAuthorized"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("FailedAuthentication"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("InvalidSecurity"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("Unauthorized"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("SecurityTokenUnavailable"), Qt::CaseInsensitive);
}

bool OnvifClient::isSoapVersionMismatch(const QByteArray &xml, int httpStatus)
{
    if (httpStatus == 415) {
        return true;
    }
    const QString text = QString::fromUtf8(xml);
    return text.contains(QStringLiteral("VersionMismatch"), Qt::CaseInsensitive);
}

} // namespace netdev
