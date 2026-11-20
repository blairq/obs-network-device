#pragma once

#include "device-types.hpp"

#include <QImage>
#include <QObject>
#include <QString>

namespace netdev {

class IDeviceProvider : public QObject {
    Q_OBJECT

public:
    explicit IDeviceProvider(QObject *parent = nullptr) : QObject(parent) {}
    ~IDeviceProvider() override = default;

    virtual DeviceProtocol protocol() const = 0;
    virtual QString displayName() const = 0;

    virtual void startScan(int timeoutMs = 3000) = 0;
    virtual void stopScan() = 0;

    virtual void inspectDevice(const QString &serviceUrl, const QString &username, const QString &password) = 0;
    virtual void fetchSnapshot(const QString &snapshotUri, const QString &username, const QString &password) = 0;

signals:
    void deviceFound(const netdev::DiscoveredDevice &device);
    void scanFinished();
    void deviceInspected(const netdev::DiscoveredDevice &device);
    void authRequired(const QString &serviceUrl, const QString &message);
    void inspectionFailed(const QString &serviceUrl, const QString &error);
    void snapshotReady(const QImage &image);
    void snapshotError(const QString &error);
};

} // namespace netdev
