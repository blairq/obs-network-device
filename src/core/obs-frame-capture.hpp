#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>

namespace netdev {

// Captures a single rendered video frame from an OBS source asynchronously without blocking the UI thread.
void captureObsSourceFrameAsync(const QString &sourceUuid,
                                QObject *context,
                                std::function<void(const QImage &)> callback);

} // namespace netdev
