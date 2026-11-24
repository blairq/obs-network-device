#include "http-snapshot-streamer.hpp"

#include <QAuthenticator>
#include <QElapsedTimer>
#include <QImage>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cstring>

namespace netdev {

class HttpSnapshotWorker : public QObject {
public:
    HttpSnapshotWorker(HttpSnapshotStreamer *owner,
                       QString url,
                       QString username,
                       QString password,
                       int maxFps)
        : m_owner(owner),
          m_url(std::move(url)),
          m_username(std::move(username)),
          m_password(std::move(password)),
          m_intervalMs(1000 / std::clamp(maxFps, 1, 30))
    {
    }

    ~HttpSnapshotWorker() override
    {
        cleanupObjects();
    }

    void begin()
    {
        if (m_stopped || !m_owner || m_owner->m_stopping.load(std::memory_order_acquire)) {
            return;
        }
        m_nam = new QNetworkAccessManager(this);
        QObject::connect(
            m_nam, &QNetworkAccessManager::authenticationRequired, this,
            [this](QNetworkReply *reply, QAuthenticator *auth) {
                if (m_stopped || !reply || !auth) {
                    return;
                }
                const int tries = reply->property("_snapAuthTries").toInt();
                if (tries >= 2 || (m_username.isEmpty() && m_password.isEmpty())) {
                    return;
                }
                reply->setProperty("_snapAuthTries", tries + 1);
                auth->setUser(m_username);
                auth->setPassword(m_password);
            });

        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        QObject::connect(m_timer, &QTimer::timeout, this, [this]() { issueRequest(); });

        issueRequest();
    }

    void shutdown()
    {
        m_stopped = true;
        cleanupObjects();
        if (QThread *th = QThread::currentThread()) {
            th->quit();
        }
    }

private:
    void cleanupObjects()
    {
        if (m_timer) {
            m_timer->stop();
        }
        if (m_activeReply) {
            QPointer<QNetworkReply> r = m_activeReply;
            m_activeReply = nullptr;
            r->disconnect(this);
            r->abort();
            if (r) {
                r->deleteLater();
            }
        }
        if (m_nam) {
            delete m_nam;
            m_nam = nullptr;
        }
        if (m_timer) {
            delete m_timer;
            m_timer = nullptr;
        }
    }

    void issueRequest()
    {
        if (m_stopped || !m_nam || !m_owner || m_owner->m_stopping.load(std::memory_order_acquire)) {
            return;
        }

        const QUrl url(m_url);
        if (!url.isValid() || url.scheme().isEmpty()) {
            return;
        }

        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(4000);
        req.setRawHeader("Connection", "keep-alive");

        QElapsedTimer elapsed;
        elapsed.start();

        QNetworkReply *reply = m_nam->get(req);
        m_activeReply = reply;

        QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, elapsed]() {
            if (m_activeReply == reply) {
                m_activeReply = nullptr;
            }
            const bool netOk = (reply->error() == QNetworkReply::NoError);
            const QByteArray payload = (netOk && reply->isOpen()) ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (m_stopped || !m_owner || m_owner->m_stopping.load(std::memory_order_acquire)) {
                return;
            }

            bool decoded = false;
            if (netOk && !payload.isEmpty()) {
                QImage img = QImage::fromData(payload);
                if (!img.isNull() && img.width() > 0 && img.height() > 0) {
                    if (img.format() != QImage::Format_RGBA8888) {
                        img = img.convertToFormat(QImage::Format_RGBA8888);
                    }
                    const uint32_t w = static_cast<uint32_t>(img.width());
                    const uint32_t h = static_cast<uint32_t>(img.height());
                    const uint32_t rowBytes = w * 4u;
                    QByteArray pixels(static_cast<qsizetype>(rowBytes) * static_cast<qsizetype>(h),
                                      Qt::Uninitialized);
                    for (uint32_t y = 0; y < h; ++y) {
                        std::memcpy(pixels.data() + static_cast<size_t>(y) * rowBytes,
                                    img.constScanLine(static_cast<int>(y)), rowBytes);
                    }
                    m_owner->publishFrame(w, h, std::move(pixels));
                    decoded = true;
                }
            }

            if (m_timer && !m_stopped && !m_owner->m_stopping.load(std::memory_order_acquire)) {
                const int waitMs = decoded
                                       ? static_cast<int>(std::max<qint64>(0, m_intervalMs - elapsed.elapsed()))
                                       : 400;
                m_timer->start(waitMs);
            }
        });
    }

    HttpSnapshotStreamer *m_owner = nullptr;
    QString m_url;
    QString m_username;
    QString m_password;
    int m_intervalMs = 66;
    bool m_stopped = false;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;
    QPointer<QNetworkReply> m_activeReply;
};

HttpSnapshotStreamer::HttpSnapshotStreamer() = default;

HttpSnapshotStreamer::~HttpSnapshotStreamer()
{
    stop();
}

void HttpSnapshotStreamer::start(const QString &url,
                                 const QString &username,
                                 const QString &password,
                                 int maxFps)
{
    stop();

    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    m_stopping.store(false, std::memory_order_release);
    m_seq.store(0, std::memory_order_release);
    m_width.store(0, std::memory_order_release);
    m_height.store(0, std::memory_order_release);
    {
        QMutexLocker locker(&m_mutex);
        m_latest = SnapshotFrame{};
    }

    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("HttpSnapshotStreamerThread"));
    m_worker = new HttpSnapshotWorker(this, trimmed, username, password, maxFps);
    m_worker->moveToThread(m_thread);

    HttpSnapshotWorker *w = m_worker;
    QObject::connect(m_thread, &QThread::started, w, [w]() { w->begin(); });
    m_thread->start();
}

void HttpSnapshotStreamer::stop()
{
    m_stopping.store(true, std::memory_order_release);
    if (m_thread) {
        if (m_worker) {
            HttpSnapshotWorker *w = m_worker;
            QMetaObject::invokeMethod(w, [w]() { w->shutdown(); }, Qt::QueuedConnection);
        }
        m_thread->quit();
        if (!m_thread->wait(3000)) {
            m_thread->terminate();
            m_thread->wait(1000);
        }
        delete m_worker;
        m_worker = nullptr;
        delete m_thread;
        m_thread = nullptr;
    }
    m_seq.store(0, std::memory_order_release);
    m_width.store(0, std::memory_order_release);
    m_height.store(0, std::memory_order_release);
    QMutexLocker locker(&m_mutex);
    m_latest = SnapshotFrame{};
}

void HttpSnapshotStreamer::publishFrame(uint32_t width, uint32_t height, QByteArray &&rgbaPixels)
{
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    const uint64_t nextSeq = m_latest.seq + 1;
    m_latest.width = width;
    m_latest.height = height;
    m_latest.seq = nextSeq;
    m_latest.rgbaPixels = std::move(rgbaPixels);
    m_width.store(width, std::memory_order_relaxed);
    m_height.store(height, std::memory_order_relaxed);
    m_seq.store(nextSeq, std::memory_order_release);
}

bool HttpSnapshotStreamer::takeLatestFrameIfNewer(uint64_t lastSeq, SnapshotFrame &out) const
{
    if (m_seq.load(std::memory_order_acquire) <= lastSeq) {
        return false;
    }
    QMutexLocker locker(&m_mutex);
    if (m_latest.seq <= lastSeq || m_latest.rgbaPixels.isEmpty()) {
        return false;
    }
    out = m_latest;
    return true;
}

void HttpSnapshotStreamer::latestDimensions(uint32_t &widthOut, uint32_t &heightOut) const
{
    widthOut = m_width.load(std::memory_order_relaxed);
    heightOut = m_height.load(std::memory_order_relaxed);
}

} // namespace netdev
