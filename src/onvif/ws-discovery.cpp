#include "ws-discovery.hpp"

#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>

namespace netdev {

namespace {

constexpr quint16 kWsDiscoveryPort = 3702;
const QHostAddress kWsDiscoveryMulticast(QStringLiteral("239.255.255.250"));

QString cleanIpString(const QHostAddress &addr)
{
    bool ok = false;
    const quint32 ipv4 = addr.toIPv4Address(&ok);
    if (ok) {
        return QHostAddress(ipv4).toString();
    }
    return addr.toString();
}

QString pickBestXAddr(const QStringList &xaddrs)
{
    QString fallback;
    for (const QString &raw : xaddrs) {
        const QUrl url(raw.trimmed());
        if (!url.isValid() || (!url.scheme().startsWith(QStringLiteral("http"), Qt::CaseInsensitive))) {
            continue;
        }
        if (fallback.isEmpty()) {
            fallback = url.toString();
        }
        const QHostAddress hostAddr(url.host());
        if (!hostAddr.isNull() && hostAddr.protocol() == QAbstractSocket::IPv4Protocol &&
            !hostAddr.isLoopback()) {
            return url.toString();
        }
    }
    return fallback;
}

} // namespace

WsDiscovery::WsDiscovery(QObject *parent) : QObject(parent)
{
    m_retryTimer.setSingleShot(true);
    m_timeoutTimer.setSingleShot(true);

    connect(&m_retryTimer, &QTimer::timeout, this, &WsDiscovery::onRetryTimeout);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &WsDiscovery::onScanTimeout);
}

WsDiscovery::~WsDiscovery()
{
    stopScan();
}

void WsDiscovery::startScan(int timeoutMs)
{
    stopScan();

    m_seenKeys.clear();
    m_scanning = true;

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack) ||
            !flags.testFlag(QNetworkInterface::CanMulticast)) {
            continue;
        }

        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback()) {
                continue;
            }

            auto *sock = new QUdpSocket(this);
            if (sock->bind(ip, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
                sock->setSocketOption(QAbstractSocket::MulticastTtlOption, 4);
                sock->setMulticastInterface(iface);
                connect(sock, &QUdpSocket::readyRead, this, &WsDiscovery::onReadyRead);
                m_sockets.push_back(sock);
            } else {
                sock->deleteLater();
            }
        }
    }

    // Fallback socket bound on AnyIPv4 in case interface-specific sockets miss replies
    auto *anySock = new QUdpSocket(this);
    if (anySock->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        anySock->setSocketOption(QAbstractSocket::MulticastTtlOption, 4);
        connect(anySock, &QUdpSocket::readyRead, this, &WsDiscovery::onReadyRead);
        m_sockets.push_back(anySock);
    } else {
        anySock->deleteLater();
    }

    sendProbesToAllSockets();

    if (timeoutMs > 900) {
        m_retryTimer.start(700);
    }
    m_timeoutTimer.start(qMax(500, timeoutMs));
}

void WsDiscovery::stopScan()
{
    m_retryTimer.stop();
    m_timeoutTimer.stop();
    cleanupSockets();
    m_scanning = false;
}

void WsDiscovery::cleanupSockets()
{
    for (QUdpSocket *sock : m_sockets) {
        if (sock) {
            sock->close();
            sock->deleteLater();
        }
    }
    m_sockets.clear();
}

void WsDiscovery::onRetryTimeout()
{
    if (m_scanning) {
        sendProbesToAllSockets();
    }
}

void WsDiscovery::onScanTimeout()
{
    if (!m_scanning) {
        return;
    }
    stopScan();
    emit scanFinished();
}

void WsDiscovery::sendProbesToAllSockets()
{
    const QByteArray nvtProbe = buildProbeXml(QStringLiteral("dn:NetworkVideoTransmitter"));
    const QByteArray tdsProbe = buildProbeXml(QStringLiteral("tds:Device"));

    for (QUdpSocket *sock : m_sockets) {
        if (!sock) {
            continue;
        }
        sock->writeDatagram(nvtProbe, kWsDiscoveryMulticast, kWsDiscoveryPort);
        sock->writeDatagram(tdsProbe, kWsDiscoveryMulticast, kWsDiscoveryPort);
    }
}

void WsDiscovery::onReadyRead()
{
    auto *sock = qobject_cast<QUdpSocket *>(sender());
    if (!sock) {
        return;
    }

    while (sock->hasPendingDatagrams()) {
        const QNetworkDatagram dgram = sock->receiveDatagram();
        if (!dgram.isValid() || dgram.data().isEmpty()) {
            continue;
        }

        const QString senderIp = cleanIpString(dgram.senderAddress());
        const QVector<DiscoveredDevice> found = parseProbeMatchXml(dgram.data(), senderIp);
        for (const DiscoveredDevice &dev : found) {
            const QString key = normalizeDeviceKey(dev);
            if (key.isEmpty() || m_seenKeys.contains(key)) {
                continue;
            }
            m_seenKeys.insert(key);
            emit deviceFound(dev);
        }
    }
}

QByteArray WsDiscovery::buildProbeXml(const QString &types)
{
    const QString msgId = QStringLiteral("urn:uuid:") +
                          QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QString xml = QStringLiteral(
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                            "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
                            "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
                            "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
                            "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" "
                            "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                            "<e:Header>"
                            "<w:MessageID>%1</w:MessageID>"
                            "<w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
                            "<w:Action e:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
                            "</e:Header>"
                            "<e:Body>"
                            "<d:Probe>"
                            "<d:Types>%2</d:Types>"
                            "</d:Probe>"
                            "</e:Body>"
                            "</e:Envelope>")
                            .arg(msgId, types);

    return xml.toUtf8();
}

QString WsDiscovery::normalizeDeviceKey(const DiscoveredDevice &dev)
{
    if (!dev.deviceServiceUrl.isEmpty()) {
        const QUrl u(dev.deviceServiceUrl);
        if (u.isValid()) {
            const int port = u.port(u.scheme() == QStringLiteral("https") ? 443 : 80);
            return QStringLiteral("%1:%2%3")
                .arg(u.host().toLower())
                .arg(port)
                .arg(u.path());
        }
    }
    return dev.id;
}

QVector<DiscoveredDevice> WsDiscovery::parseProbeMatchXml(const QByteArray &xml, const QString &senderIp)
{
    QVector<DiscoveredDevice> result;
    QXmlStreamReader reader(xml);

    bool inProbeMatch = false;
    bool inEndpointRef = false;
    DiscoveredDevice current;
    QString eprAddress;
    QString scopesText;
    QString xaddrsText;

    const auto finalizeMatch = [&]() {
        const QRegularExpression wsRe(QStringLiteral("\\s+"));
        const QStringList xaddrs = xaddrsText.trimmed().split(wsRe, Qt::SkipEmptyParts);
        const QString bestXAddr = pickBestXAddr(xaddrs);

        if (bestXAddr.isEmpty() && eprAddress.isEmpty()) {
            return;
        }

        current.protocol = DeviceProtocol::Onvif;
        current.deviceServiceUrl = bestXAddr;
        current.id = !bestXAddr.isEmpty() ? bestXAddr : eprAddress;

        const QUrl url(bestXAddr);
        if (url.isValid() && !url.host().isEmpty()) {
            current.ip = url.host();
            const int p = url.port(url.scheme() == QStringLiteral("https") ? 443 : 80);
            current.port = static_cast<quint16>(qBound(1, p, 65535));
        } else {
            current.ip = senderIp;
            current.port = 80;
            if (!senderIp.isEmpty() && current.deviceServiceUrl.isEmpty()) {
                current.deviceServiceUrl =
                    QStringLiteral("http://%1/onvif/device_service").arg(senderIp);
                current.id = current.deviceServiceUrl;
            }
        }

        const QStringList scopes = scopesText.trimmed().split(wsRe, Qt::SkipEmptyParts);
        for (const QString &scope : scopes) {
            const int nameIdx = scope.indexOf(QStringLiteral("/name/"), 0, Qt::CaseInsensitive);
            const int hwIdx = scope.indexOf(QStringLiteral("/hardware/"), 0, Qt::CaseInsensitive);
            if (scope.contains(QStringLiteral("onvif://www.onvif.org/name/"), Qt::CaseInsensitive) &&
                nameIdx >= 0) {
                const QString rawName = scope.mid(nameIdx + 6);
                const QString decoded = QUrl::fromPercentEncoding(rawName.toUtf8()).trimmed();
                if (!decoded.isEmpty()) {
                    current.name = decoded;
                }
            } else if (scope.contains(QStringLiteral("onvif://www.onvif.org/hardware/"), Qt::CaseInsensitive) &&
                       hwIdx >= 0) {
                const QString rawHw = scope.mid(hwIdx + 10);
                const QString decoded = QUrl::fromPercentEncoding(rawHw.toUtf8()).trimmed();
                if (!decoded.isEmpty()) {
                    current.model = decoded;
                }
            } else if (scope.contains(QStringLiteral("/type/ptz"), Qt::CaseInsensitive)) {
                current.hasPtz = true;
            }
        }

        if (current.name.isEmpty()) {
            current.name = !current.model.isEmpty()
                               ? current.model
                               : QStringLiteral("ONVIF (%1)").arg(current.ip);
        }

        result.push_back(current);
    };

    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringView local = reader.name();
            if (local == QLatin1String("ProbeMatch")) {
                inProbeMatch = true;
                inEndpointRef = false;
                current = DiscoveredDevice{};
                eprAddress.clear();
                scopesText.clear();
                xaddrsText.clear();
            } else if (inProbeMatch) {
                if (local == QLatin1String("EndpointReference")) {
                    inEndpointRef = true;
                } else if (inEndpointRef && local == QLatin1String("Address")) {
                    eprAddress = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                } else if (local == QLatin1String("Scopes")) {
                    scopesText += QLatin1Char(' ') +
                                  reader.readElementText(QXmlStreamReader::SkipChildElements);
                } else if (local == QLatin1String("XAddrs")) {
                    xaddrsText += QLatin1Char(' ') +
                                  reader.readElementText(QXmlStreamReader::SkipChildElements);
                }
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QStringView local = reader.name();
            if (local == QLatin1String("EndpointReference")) {
                inEndpointRef = false;
            } else if (local == QLatin1String("ProbeMatch") && inProbeMatch) {
                finalizeMatch();
                inProbeMatch = false;
            }
        }
    }

    return result;
}

} // namespace netdev
