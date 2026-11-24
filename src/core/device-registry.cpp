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

    bool changed = false;
    {
        QMutexLocker lock(&m_mutex);
        bool found = false;
        for (PtzCameraEntry &existing : m_cameras) {
            if (existing.sourceUuid == entry.sourceUuid) {
                found = true;
                if (existing.sourceName != entry.sourceName ||
                    existing.deviceName != entry.deviceName ||
                    existing.manufacturer != entry.manufacturer ||
                    existing.model != entry.model ||
                    existing.serialNumber != entry.serialNumber ||
                    existing.deviceServiceUrl != entry.deviceServiceUrl ||
                    existing.ptzServiceUrl != entry.ptzServiceUrl ||
                    existing.snapshotUri != entry.snapshotUri ||
                    existing.profileToken != entry.profileToken ||
                    existing.username != entry.username ||
                    existing.password != entry.password ||
                    existing.clockOffsetSecs != entry.clockOffsetSecs ||
                    existing.cameraKey != entry.cameraKey ||
                    existing.fallbackHostKey != entry.fallbackHostKey) {
                    existing = entry;
                    changed = true;
                }
                break;
            }
        }
        if (!found) {
            m_cameras.push_back(entry);
            changed = true;
        }
    }

    if (changed) {
        emit cameraListChanged();
    }
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
