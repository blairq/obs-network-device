#include "panorama-scanner.hpp"

#include "core/obs-frame-capture.hpp"
#include "onvif/onvif-client.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace netdev {

namespace {

constexpr int kPollIntervalMs = 300;
constexpr int kInitialStatusWaitMs = 500;
constexpr int kPostStopSettleMs = 300;

} // namespace

PanoramaScanner::PanoramaScanner(OnvifClient *client, QObject *parent)
    : QObject(parent), m_client(client)
{
}

PanoramaScanner::~PanoramaScanner()
{
    ++m_generation;
    m_running = false;
    stopWorkerThread();
}

void PanoramaScanner::stopWorkerThread()
{
    if (m_stitchThread.joinable()) {
        m_stitchThread.join();
    }
}

void PanoramaScanner::startScan(const PtzCameraEntry &cam, const PanoramaScanSettings &settings)
{
    if (!m_client || cam.ptzServiceUrl.isEmpty() || cam.profileToken.isEmpty()) {
        emit scanFinished(false, QImage(), PanoramaMeta{},
                          QStringLiteral("Camara PTZ no configurada correctamente."));
        return;
    }

    if (m_running) {
        cancelScan();
    }
    stopWorkerThread();

    const quint64 gen = ++m_generation;
    m_running = true;
    m_cam = cam;
    m_settings = settings;
    m_settings.cols = std::clamp(m_settings.cols, 1, 12);
    m_settings.rows = std::clamp(m_settings.rows, 1, 8);
    m_settings.settleDelayMs = std::clamp(m_settings.settleDelayMs, 250, 5000);
    m_settings.moveTimeoutMs = std::clamp(m_settings.moveTimeoutMs, 1000, 10000);
    m_capturedTiles.clear();
    m_targets.clear();
    m_currentIndex = 0;
    m_hasInitialPos = false;
    m_initialPos = PtzPosition{};

    const int cols = m_settings.cols;
    const int rows = m_settings.rows;
    const double minPan = std::min(m_settings.panMin, m_settings.panMax);
    const double maxPan = std::max(m_settings.panMin, m_settings.panMax);
    const double minTilt = std::min(m_settings.tiltMin, m_settings.tiltMax);
    const double maxTilt = std::max(m_settings.tiltMin, m_settings.tiltMax);
    m_settings.panMin = minPan;
    m_settings.panMax = maxPan;
    m_settings.tiltMin = minTilt;
    m_settings.tiltMax = maxTilt;

    m_targets.reserve(cols * rows);
    for (int r = 0; r < rows; ++r) {
        const double rowFrac = (rows > 1) ? static_cast<double>(r) / static_cast<double>(rows - 1) : 0.5;
        const double tilt = m_settings.invertTilt
                                ? (minTilt + rowFrac * (maxTilt - minTilt))
                                : (maxTilt - rowFrac * (maxTilt - minTilt));
        for (int cIdx = 0; cIdx < cols; ++cIdx) {
            const int c = (r % 2 == 0) ? cIdx : (cols - 1 - cIdx);
            const double colFrac =
                (cols > 1) ? static_cast<double>(c) / static_cast<double>(cols - 1) : 0.5;
            const double pan = m_settings.invertPan
                                   ? (maxPan - colFrac * (maxPan - minPan))
                                   : (minPan + colFrac * (maxPan - minPan));
            m_targets.push_back(GridTarget{c, r, pan, tilt});
        }
    }

    emit progressUpdated(0, static_cast<int>(m_targets.size()),
                         QStringLiteral("Leyendo posicion inicial de la camara..."), QImage());

    QPointer<PanoramaScanner> self(this);
    m_client->getPtzStatus(
        m_cam.ptzServiceUrl, m_cam.profileToken, m_cam.username, m_cam.password,
        m_cam.clockOffsetSecs,
        [self, gen](bool ok, const PtzStatus &status, const QString &) {
            if (!self || !self->m_running || gen != self->m_generation) {
                return;
            }
            if (ok && status.valid && status.position.hasPanTilt) {
                self->m_hasInitialPos = true;
                self->m_initialPos = status.position;
            }
            self->beginStep(gen);
        });
}

void PanoramaScanner::cancelScan()
{
    if (!m_running) {
        return;
    }
    ++m_generation;
    m_running = false;
    if (m_client && !m_cam.ptzServiceUrl.isEmpty() && !m_cam.profileToken.isEmpty()) {
        m_client->stopPtz(m_cam.ptzServiceUrl, m_cam.profileToken, m_cam.username,
                          m_cam.password, m_cam.clockOffsetSecs, nullptr);
    }
    restoreInitialPosition();
    emit scanCancelled();
}

void PanoramaScanner::beginStep(quint64 gen)
{
    if (!m_running || gen != m_generation) {
        return;
    }
    if (m_currentIndex >= static_cast<int>(m_targets.size())) {
        startStitching(gen);
        return;
    }

    const GridTarget target = m_targets.at(m_currentIndex);
    const int total = static_cast<int>(m_targets.size());
    emit progressUpdated(
        m_currentIndex, total,
        QStringLiteral("Moviendo a punto %1/%2 (P=%3, T=%4)...")
            .arg(m_currentIndex + 1)
            .arg(total)
            .arg(target.pan, 0, 'f', 2)
            .arg(target.tilt, 0, 'f', 2),
        QImage());

    QPointer<PanoramaScanner> self(this);
    m_client->absoluteMove(
        m_cam.ptzServiceUrl, m_cam.profileToken, m_cam.username, m_cam.password,
        m_cam.clockOffsetSecs, target.pan, target.tilt, m_settings.scanZoom, true, 0.85,
        [self, gen, target](bool ok, const QString &err) {
            if (!self || !self->m_running || gen != self->m_generation) {
                return;
            }
            if (!ok && self->m_currentIndex == 0) {
                // Retry once without zoom in case camera restricts AbsoluteMove Zoom
                self->m_client->absoluteMove(
                    self->m_cam.ptzServiceUrl, self->m_cam.profileToken, self->m_cam.username,
                    self->m_cam.password, self->m_cam.clockOffsetSecs, target.pan, target.tilt,
                    0.0, false, 0.85,
                    [self, gen, err](bool okNoZoom, const QString &err2) {
                        if (!self || !self->m_running || gen != self->m_generation) {
                            return;
                        }
                        if (!okNoZoom) {
                            self->m_running = false;
                            emit self->scanFinished(
                                false, QImage(), PanoramaMeta{},
                                QStringLiteral("La camara no acepto AbsoluteMove ONVIF: %1")
                                    .arg(err2.isEmpty() ? err : err2));
                            return;
                        }
                        self->m_moveTimer.restart();
                        QTimer::singleShot(kInitialStatusWaitMs, self.data(), [self, gen]() {
                            if (self && self->m_running && gen == self->m_generation) {
                                self->pollStatusUntilSettled(gen, PtzPosition{}, false);
                            }
                        });
                    });
                return;
            }

            self->m_moveTimer.restart();
            QTimer::singleShot(kInitialStatusWaitMs, self.data(), [self, gen]() {
                if (self && self->m_running && gen == self->m_generation) {
                    self->pollStatusUntilSettled(gen, PtzPosition{}, false);
                }
            });
        });
}

void PanoramaScanner::pollStatusUntilSettled(quint64 gen,
                                             const PtzPosition &lastPos,
                                             bool hasLastPos)
{
    Q_UNUSED(lastPos);
    Q_UNUSED(hasLastPos);
    if (!m_running || gen != m_generation || m_currentIndex >= static_cast<int>(m_targets.size())) {
        return;
    }

    const GridTarget target = m_targets.at(m_currentIndex);
    QPointer<PanoramaScanner> self(this);

    m_client->getPtzStatus(
        m_cam.ptzServiceUrl, m_cam.profileToken, m_cam.username, m_cam.password,
        m_cam.clockOffsetSecs,
        [self, gen, target](bool ok, const PtzStatus &status, const QString &) {
            if (!self || !self->m_running || gen != self->m_generation) {
                return;
            }

            double settledPan = target.pan;
            double settledTilt = target.tilt;
            if (ok && status.valid && status.position.hasPanTilt) {
                const bool reasonable =
                    std::abs(status.position.pan - target.pan) <= 0.25 &&
                    std::abs(status.position.tilt - target.tilt) <= 0.25 &&
                    status.position.pan >= self->m_settings.panMin - 0.15 &&
                    status.position.pan <= self->m_settings.panMax + 0.15 &&
                    status.position.tilt >= self->m_settings.tiltMin - 0.15 &&
                    status.position.tilt <= self->m_settings.tiltMax + 0.15;
                if (reasonable) {
                    settledPan = status.position.pan;
                    settledTilt = status.position.tilt;
                }
            }

            const qint64 elapsed = self->m_moveTimer.elapsed();
            if (!ok) {
                const int remaining = std::max(50, self->m_settings.settleDelayMs - static_cast<int>(elapsed));
                QTimer::singleShot(remaining, self.data(), [self, gen, settledPan, settledTilt]() {
                    self->captureCurrentTile(gen, settledPan, settledTilt);
                });
                return;
            }

            const bool moving = status.panTiltMoving || status.zoomMoving;
            if (!moving || elapsed >= self->m_settings.moveTimeoutMs) {
                QTimer::singleShot(kPostStopSettleMs, self.data(), [self, gen, settledPan, settledTilt]() {
                    self->captureCurrentTile(gen, settledPan, settledTilt);
                });
                return;
            }

            QTimer::singleShot(kPollIntervalMs, self.data(), [self, gen, status]() {
                if (self && self->m_running && gen == self->m_generation) {
                    self->pollStatusUntilSettled(gen, status.position, status.position.hasPanTilt);
                }
            });
        });
}

void PanoramaScanner::captureCurrentTile(quint64 gen, double pan, double tilt)
{
    if (!m_running || gen != m_generation || m_currentIndex >= static_cast<int>(m_targets.size())) {
        return;
    }

    const GridTarget target = m_targets.at(m_currentIndex);
    if (m_settings.captureMode == FrameCaptureMode::ObsSourceOnly ||
        (m_settings.captureMode == FrameCaptureMode::AutoOnvifThenObs &&
         m_cam.snapshotUri.trimmed().isEmpty())) {
        captureFromObsFallback(gen, target, pan, tilt);
        return;
    }

    QPointer<PanoramaScanner> self(this);
    m_client->fetchSnapshotToCallback(
        m_cam.snapshotUri, m_cam.username, m_cam.password,
        [self, gen, target, pan, tilt](bool ok, const QImage &image, const QString &) {
            if (!self || !self->m_running || gen != self->m_generation) {
                return;
            }
            if (ok && !image.isNull()) {
                self->recordCapturedTile(gen, target, pan, tilt, image);
                return;
            }
            if (self->m_settings.captureMode == FrameCaptureMode::AutoOnvifThenObs) {
                self->captureFromObsFallback(gen, target, pan, tilt);
                return;
            }
            self->recordCapturedTile(gen, target, pan, tilt, QImage());
        });
}

void PanoramaScanner::captureFromObsFallback(quint64 gen,
                                             const GridTarget &target,
                                             double pan,
                                             double tilt)
{
    if (!m_running || gen != m_generation) {
        return;
    }

    QPointer<PanoramaScanner> self(this);
    captureObsSourceFrameAsync(
        m_cam.sourceUuid, this,
        [self, gen, target, pan, tilt](const QImage &frameImg) {
            if (!self || !self->m_running || gen != self->m_generation) {
                return;
            }
            self->recordCapturedTile(gen, target, pan, tilt, frameImg);
        });
}

void PanoramaScanner::recordCapturedTile(quint64 gen,
                                         const GridTarget &target,
                                         double pan,
                                         double tilt,
                                         const QImage &image)
{
    if (!m_running || gen != m_generation) {
        return;
    }

    if (!image.isNull()) {
        CapturedTile tile;
        tile.col = target.col;
        tile.row = target.row;
        tile.pan = pan;
        tile.tilt = tilt;
        tile.image = image;
        m_capturedTiles.push_back(std::move(tile));
    }

    ++m_currentIndex;
    const int total = static_cast<int>(m_targets.size());
    emit progressUpdated(
        m_currentIndex, total,
        QStringLiteral("Capturado %1/%2 (%3 valido(s))...")
            .arg(m_currentIndex)
            .arg(total)
            .arg(m_capturedTiles.size()),
        image);

    if (m_currentIndex >= total) {
        startStitching(gen);
    } else {
        beginStep(gen);
    }
}

void PanoramaScanner::startStitching(quint64 gen)
{
    if (!m_running || gen != m_generation) {
        return;
    }

    const int total = static_cast<int>(m_targets.size());
    emit progressUpdated(total, total, QStringLiteral("Uniendo y alineando panoramica..."), QImage());

    restoreInitialPosition();
    stopWorkerThread();

    QVector<CapturedTile> tilesCopy = m_capturedTiles;
    PanoramaScanSettings settingsCopy = m_settings;
    QPointer<PanoramaScanner> self(this);

    m_stitchThread = std::thread([self, gen, tiles = std::move(tilesCopy), settings = settingsCopy]() {
        PanoramaBuildResult res = PanoramaBuilder::build(tiles, settings);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, gen, res = std::move(res)]() mutable {
                if (!self || gen != self->m_generation) {
                    return;
                }
                self->m_running = false;
                if (res.ok) {
                    emit self->scanFinished(true, res.image, res.meta, QString());
                } else {
                    emit self->scanFinished(false, QImage(), PanoramaMeta{}, res.error);
                }
            },
            Qt::QueuedConnection);
    });
}

void PanoramaScanner::restoreInitialPosition()
{
    if (!m_hasInitialPos || !m_client || m_cam.ptzServiceUrl.isEmpty() ||
        m_cam.profileToken.isEmpty()) {
        return;
    }
    m_hasInitialPos = false;
    m_client->absoluteMove(
        m_cam.ptzServiceUrl, m_cam.profileToken, m_cam.username, m_cam.password,
        m_cam.clockOffsetSecs, m_initialPos.pan, m_initialPos.tilt, m_initialPos.zoom,
        m_initialPos.hasZoom, 0.75, nullptr);
}

} // namespace netdev
