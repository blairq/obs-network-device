#include "device-registry.hpp"

#include <QMutexLocker>

namespace netdev {

DeviceRegistry &DeviceRegistry::instance()
{
    static DeviceRegistry inst;
    return inst;
}

DeviceRegistry::DeviceRegistry(QObject *parent) : QObject(parent) {}

void DeviceRegistry::registerCamera(const PtzCameraEntry &entry)
{
    if (entry.sourceUuid.isEmpty() || entry.ptzServiceUrl.isEmpty() || entry.profileToken.isEmpty()) {
        return;
    }

    {
        QMutexLocker lock(&m_mutex);
        bool updated = false;
        for (PtzCameraEntry &existing : m_cameras) {
            if (existing.sourceUuid == entry.sourceUuid) {
                existing = entry;
                updated = true;
                break;
            }
        }
        if (!updated) {
            m_cameras.push_back(entry);
        }
    }

    emit cameraListChanged();
}

void DeviceRegistry::unregisterCamera(const QString &sourceUuid)
{
    if (sourceUuid.isEmpty()) {
        return;
    }

    bool removed = false;
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < m_cameras.size(); ++i) {
            if (m_cameras.at(i).sourceUuid == sourceUuid) {
                m_cameras.removeAt(i);
                removed = true;
                break;
            }
        }
    }

    if (removed) {
        emit cameraListChanged();
    }
}

QVector<PtzCameraEntry> DeviceRegistry::cameras() const
{
    QMutexLocker lock(&m_mutex);
    return m_cameras;
}

bool DeviceRegistry::cameraById(const QString &sourceUuid, PtzCameraEntry &out) const
{
    QMutexLocker lock(&m_mutex);
    for (const PtzCameraEntry &entry : m_cameras) {
        if (entry.sourceUuid == sourceUuid) {
            out = entry;
            return true;
        }
    }
    return false;
}

bool DeviceRegistry::findBySourceName(const QString &sourceName, PtzCameraEntry &out) const
{
    QMutexLocker lock(&m_mutex);
    for (const PtzCameraEntry &entry : m_cameras) {
        if (entry.sourceName == sourceName) {
            out = entry;
            return true;
        }
    }
    return false;
}

void DeviceRegistry::notifyActiveSourceChanged(const QString &sourceName)
{
    emit activeSourceChanged(sourceName);
}

} // namespace netdev
