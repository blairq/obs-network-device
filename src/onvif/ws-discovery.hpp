#pragma once

#include "core/device-types.hpp"

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>

namespace netdev {

class WsDiscovery : public QObject {
    Q_OBJECT

public:
    explicit WsDiscovery(QObject *parent = nullptr);
    ~WsDiscovery() override;

    void startScan(int timeoutMs = 3000);
    void stopScan();
    bool isScanning() const { return m_scanning; }

    static QVector<DiscoveredDevice> parseProbeMatchXml(const QByteArray &xml,
                                                        const QString &senderIp = QString());

signals:
    void deviceFound(const netdev::DiscoveredDevice &device);
    void scanFinished();

private slots:
    void onReadyRead();
    void onRetryTimeout();
    void onScanTimeout();

private:
    void sendProbesToAllSockets();
    void cleanupSockets();
    static QByteArray buildProbeXml(const QString &types);
    static QString normalizeDeviceKey(const DiscoveredDevice &dev);

    QVector<QUdpSocket *> m_sockets;
    QSet<QString> m_seenKeys;
    QTimer m_retryTimer;
    QTimer m_timeoutTimer;
    bool m_scanning = false;
};

} // namespace netdev
