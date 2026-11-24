#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>

#include <atomic>
#include <cstdint>

class QThread;

namespace netdev {

struct SnapshotFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t seq = 0;
    QByteArray rgbaPixels;
};

class HttpSnapshotWorker;

class HttpSnapshotStreamer {
public:
    HttpSnapshotStreamer();
    ~HttpSnapshotStreamer();

    HttpSnapshotStreamer(const HttpSnapshotStreamer &) = delete;
    HttpSnapshotStreamer &operator=(const HttpSnapshotStreamer &) = delete;

    void start(const QString &url, const QString &username, const QString &password, int maxFps);
    void stop();

    bool takeLatestFrameIfNewer(uint64_t lastSeq, SnapshotFrame &out) const;
    void latestDimensions(uint32_t &widthOut, uint32_t &heightOut) const;

private:
    friend class HttpSnapshotWorker;

    void publishFrame(uint32_t width, uint32_t height, QByteArray &&rgbaPixels);

    mutable QMutex m_mutex;
    SnapshotFrame m_latest;
    std::atomic<uint64_t> m_seq{0};
    std::atomic<uint32_t> m_width{0};
    std::atomic<uint32_t> m_height{0};
    std::atomic<bool> m_stopping{false};

    QThread *m_thread = nullptr;
    HttpSnapshotWorker *m_worker = nullptr;
};

} // namespace netdev
