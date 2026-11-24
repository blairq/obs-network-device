#pragma once

#include "core/device-types.hpp"

#include <QImage>
#include <QString>
#include <QVector>

namespace netdev {

struct CapturedTile {
    int col = 0;
    int row = 0;
    double pan = 0.0;
    double tilt = 0.0;
    QImage image;
};

struct PanoramaBuildResult {
    bool ok = false;
    QImage image;
    PanoramaMeta meta;
    QString error;
};

class PanoramaBuilder {
public:
    static PanoramaBuildResult build(const QVector<CapturedTile> &tiles,
                                     const PanoramaScanSettings &settings);
};

class PanoramaMapper {
public:
    static bool pixelToPanTilt(const PanoramaMeta &meta,
                               double px,
                               double py,
                               double &panOut,
                               double &tiltOut);

    static bool panTiltToPixel(const PanoramaMeta &meta,
                               double pan,
                               double tilt,
                               double &pxOut,
                               double &pyOut);
};

} // namespace netdev
