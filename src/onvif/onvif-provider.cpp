#include "onvif-provider.hpp"

namespace netdev {

OnvifProvider::OnvifProvider(QObject *parent)
    : IDeviceProvider(parent), m_discovery(this), m_client(this)
{
    connect(&m_discovery, &WsDiscovery::deviceFound, this, &IDeviceProvider::deviceFound);
    connect(&m_discovery, &WsDiscovery::scanFinished, this, &IDeviceProvider::scanFinished);
    connect(&m_client, &OnvifClient::deviceInspected, this, &IDeviceProvider::deviceInspected);
    connect(&m_client, &OnvifClient::authRequired, this, &IDeviceProvider::authRequired);
    connect(&m_client, &OnvifClient::inspectionFailed, this, &IDeviceProvider::inspectionFailed);
    connect(&m_client, &OnvifClient::snapshotReady, this, &IDeviceProvider::snapshotReady);
    connect(&m_client, &OnvifClient::snapshotError, this, &IDeviceProvider::snapshotError);
}

void OnvifProvider::startScan(int timeoutMs)
{
    m_discovery.startScan(timeoutMs);
}

void OnvifProvider::stopScan()
{
    m_discovery.stopScan();
}

void OnvifProvider::inspectDevice(const QString &serviceUrl, const QString &username, const QString &password)
{
    m_client.inspectDevice(serviceUrl, username, password);
}

void OnvifProvider::fetchSnapshot(const QString &snapshotUri, const QString &username, const QString &password)
{
    m_client.fetchSnapshot(snapshotUri, username, password);
}

} // namespace netdev
