#pragma once

#include "core/device-provider.hpp"
#include "onvif-client.hpp"
#include "ws-discovery.hpp"

namespace netdev {

class OnvifProvider : public IDeviceProvider {
    Q_OBJECT

public:
    explicit OnvifProvider(QObject *parent = nullptr);
    ~OnvifProvider() override = default;

    DeviceProtocol protocol() const override { return DeviceProtocol::Onvif; }
    QString displayName() const override { return QStringLiteral("ONVIF"); }

    void startScan(int timeoutMs = 3000) override;
    void stopScan() override;

    void inspectDevice(const QString &serviceUrl, const QString &username, const QString &password) override;
    void fetchSnapshot(const QString &snapshotUri, const QString &username, const QString &password) override;

    OnvifClient *client() { return &m_client; }

private:
    WsDiscovery m_discovery;
    OnvifClient m_client;
};

} // namespace netdev
