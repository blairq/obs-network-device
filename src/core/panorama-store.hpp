#pragma once

#include "core/device-types.hpp"

#include <QImage>
#include <QString>

namespace netdev {

class PanoramaStore {
public:
    explicit PanoramaStore(QString baseDir);

    QString baseDir() const { return m_baseDir; }
    QString cameraDir(const QString &cameraKey) const;

    bool loadProfile(const QString &cameraKey,
                     const QString &fallbackHostKey,
                     CameraPtzProfile &profileOut,
                     QImage &panoramaOut) const;

    bool saveProfile(const CameraPtzProfile &profile) const;
    bool savePanorama(CameraPtzProfile &profile, const QImage &image, const PanoramaMeta &meta) const;

    QString savePresetThumbnail(const QString &cameraKey,
                                const QString &presetId,
                                const QImage &image) const;
    QImage loadPresetThumbnail(const QString &cameraKey, const QString &relativePath) const;
    void removePresetThumbnail(const QString &cameraKey, const QString &relativePath) const;

private:
    QString resolveExistingCameraDir(const QString &cameraKey, const QString &fallbackHostKey) const;

    QString m_baseDir;
};

} // namespace netdev
