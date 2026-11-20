#pragma once

#include "device-types.hpp"

#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

namespace netdev {

class DeviceRegistry : public QObject {
    Q_OBJECT

public:
    static DeviceRegistry &instance();

    void registerCamera(const PtzCameraEntry &entry);
    void unregisterCamera(const QString &sourceUuid);

    QVector<PtzCameraEntry> cameras() const;
    bool cameraById(const QString &sourceUuid, PtzCameraEntry &out) const;
    bool findBySourceName(const QString &sourceName, PtzCameraEntry &out) const;

    void notifyActiveSourceChanged(const QString &sourceName);

signals:
    void cameraListChanged();
    void activeSourceChanged(const QString &sourceName);

private:
    explicit DeviceRegistry(QObject *parent = nullptr);
    ~DeviceRegistry() override = default;

    mutable QMutex m_mutex;
    QVector<PtzCameraEntry> m_cameras;
};

} // namespace netdev
