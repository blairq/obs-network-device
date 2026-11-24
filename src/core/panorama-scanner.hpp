#pragma once

#include "core/device-types.hpp"
#include "core/panorama-builder.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

#include <thread>

namespace netdev {

class OnvifClient;

class PanoramaScanner : public QObject {
    Q_OBJECT

public:
    explicit PanoramaScanner(OnvifClient *client, QObject *parent = nullptr);
    ~PanoramaScanner() override;

    bool isRunning() const { return m_running; }

    void startScan(const PtzCameraEntry &cam, const PanoramaScanSettings &settings);
    void cancelScan();

signals:
    void progressUpdated(int completed, int total, const QString &statusText, const QImage &lastTile);
    void scanFinished(bool ok, const QImage &panorama, const PanoramaMeta &meta, const QString &error);
    void scanCancelled();

private:
    struct GridTarget {
        int col = 0;
        int row = 0;
        double pan = 0.0;
        double tilt = 0.0;
    };

    void stopWorkerThread();
    void beginStep(quint64 gen);
    void pollStatusUntilSettled(quint64 gen, const PtzPosition &lastPos, bool hasLastPos);
    void captureCurrentTile(quint64 gen, double pan, double tilt);
    void captureFromObsFallback(quint64 gen, const GridTarget &target, double pan, double tilt);
    void recordCapturedTile(quint64 gen, const GridTarget &target, double pan, double tilt,
                            const QImage &image);
    void startStitching(quint64 gen);
    void restoreInitialPosition();

    OnvifClient *m_client = nullptr;
    bool m_running = false;
    quint64 m_generation = 0;

    PtzCameraEntry m_cam;
    PanoramaScanSettings m_settings;
    QVector<GridTarget> m_targets;
    QVector<CapturedTile> m_capturedTiles;
    int m_currentIndex = 0;

    bool m_hasInitialPos = false;
    PtzPosition m_initialPos;

    QElapsedTimer m_moveTimer;
    std::thread m_stitchThread;
};

} // namespace netdev
