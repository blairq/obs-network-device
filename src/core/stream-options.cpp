#include "stream-options.hpp"

#include "onvif/onvif-auth.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace netdev {
namespace {

QString formatCodecLabel(const QString &encoding)
{
    const QString upper = encoding.trimmed().toUpper();
    if (upper == QLatin1String("JPEG") || upper == QLatin1String("MJPEG")) {
        return QStringLiteral("MJPEG");
    }
    return encoding.trimmed();
}

QString extractAudioHint(const QString &uri, const QString &fallbackEncoding, bool hasAudio)
{
    const QUrl url(uri);
    if (url.isValid() && url.hasQuery()) {
        const QUrlQuery q(url);
        const QString a = q.queryItemValue(QStringLiteral("audio")).trimmed();
        if (!a.isEmpty()) {
            return a.toUpper();
        }
    }
    if (!fallbackEncoding.trimmed().isEmpty()) {
        return fallbackEncoding.trimmed().toUpper();
    }
    return hasAudio ? QStringLiteral("Audio") : QString();
}

QString buildProfileSpec(const MediaProfile &p)
{
    QStringList parts;
    const QString codec = formatCodecLabel(p.videoEncoding);
    if (!codec.isEmpty()) {
        parts << codec;
    }
    if (p.width > 0 && p.height > 0) {
        parts << QStringLiteral("%1x%2").arg(p.width).arg(p.height);
    }
    if (p.fps > 0.0) {
        parts << QStringLiteral("%1fps").arg(qRound(p.fps));
    }
    return parts.join(QLatin1Char(' '));
}

QString formatEntryLabel(const MediaProfile &p, const QString &modeSuffix)
{
    const QString baseName = !p.name.trimmed().isEmpty() ? p.name.trimmed() : p.token.trimmed();
    const QString spec = buildProfileSpec(p);
    QString head = baseName;
    if (!spec.isEmpty()) {
        head += QStringLiteral(" — %1").arg(spec);
    }
    if (!modeSuffix.isEmpty()) {
        head += QStringLiteral(" [%1]").arg(modeSuffix);
    }
    return head;
}

} // namespace

QString serializeMediaProfiles(const QVector<MediaProfile> &profiles)
{
    QJsonArray arr;
    for (const MediaProfile &p : profiles) {
        QJsonObject obj;
        obj.insert(QStringLiteral("token"), p.token);
        obj.insert(QStringLiteral("name"), p.name);
        obj.insert(QStringLiteral("videoEncoding"), p.videoEncoding);
        obj.insert(QStringLiteral("audioEncoding"), p.audioEncoding);
        obj.insert(QStringLiteral("width"), p.width);
        obj.insert(QStringLiteral("height"), p.height);
        obj.insert(QStringLiteral("fps"), p.fps);
        obj.insert(QStringLiteral("hasAudio"), p.hasAudio);
        obj.insert(QStringLiteral("hasPtz"), p.hasPtz);
        obj.insert(QStringLiteral("isMedia2"), p.isMedia2);
        obj.insert(QStringLiteral("ptzToken"), p.ptzToken);
        obj.insert(QStringLiteral("rtspUri"), p.rtspUri);
        obj.insert(QStringLiteral("media2RtspUri"), p.media2RtspUri);
        obj.insert(QStringLiteral("httpUri"), p.httpUri);
        obj.insert(QStringLiteral("snapshotUri"), p.snapshotUri);
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<MediaProfile> deserializeMediaProfiles(const QString &json)
{
    QVector<MediaProfile> out;
    if (json.trimmed().isEmpty()) {
        return out;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return out;
    }

    const QJsonArray arr = doc.array();
    out.reserve(arr.size());
    for (const QJsonValue &val : arr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        MediaProfile p;
        p.token = obj.value(QStringLiteral("token")).toString();
        if (p.token.isEmpty()) {
            continue;
        }
        p.name = obj.value(QStringLiteral("name")).toString();
        p.videoEncoding = obj.value(QStringLiteral("videoEncoding")).toString();
        p.audioEncoding = obj.value(QStringLiteral("audioEncoding")).toString();
        p.width = obj.value(QStringLiteral("width")).toInt(0);
        p.height = obj.value(QStringLiteral("height")).toInt(0);
        p.fps = obj.value(QStringLiteral("fps")).toDouble(0.0);
        p.hasAudio = obj.value(QStringLiteral("hasAudio")).toBool(false);
        p.hasPtz = obj.value(QStringLiteral("hasPtz")).toBool(false);
        p.isMedia2 = obj.value(QStringLiteral("isMedia2")).toBool(false);
        p.ptzToken = obj.value(QStringLiteral("ptzToken")).toString();
        p.rtspUri = obj.value(QStringLiteral("rtspUri")).toString();
        p.media2RtspUri = obj.value(QStringLiteral("media2RtspUri")).toString();
        p.httpUri = obj.value(QStringLiteral("httpUri")).toString();
        p.snapshotUri = obj.value(QStringLiteral("snapshotUri")).toString();
        out.push_back(std::move(p));
    }
    return out;
}

QVector<StreamOptionEntry> buildSelectableStreamOptions(const QVector<MediaProfile> &profiles)
{
    QVector<StreamOptionEntry> entries;
    for (const MediaProfile &p : profiles) {
        if (p.token.isEmpty()) {
            continue;
        }
        const QString profName = !p.name.trimmed().isEmpty() ? p.name.trimmed() : p.token;

        if (!p.rtspUri.trimmed().isEmpty()) {
            const QString audioHint = extractAudioHint(p.rtspUri, p.audioEncoding, p.hasAudio);
            const QString suffix = audioHint.isEmpty()
                                       ? QStringLiteral("RTSP")
                                       : QStringLiteral("RTSP / %1").arg(audioHint);
            StreamOptionEntry e;
            e.id = QStringLiteral("rtsp::%1").arg(p.token);
            e.label = formatEntryLabel(p, suffix);
            e.profileToken = p.token;
            e.profileName = profName;
            e.uri = p.rtspUri.trimmed();
            e.snapshotUri = p.snapshotUri.trimmed();
            entries.push_back(std::move(e));
        }

        if (!p.media2RtspUri.trimmed().isEmpty() && p.media2RtspUri.trimmed() != p.rtspUri.trimmed()) {
            const QString audioHint = extractAudioHint(p.media2RtspUri, p.audioEncoding, p.hasAudio);
            const QString suffix = audioHint.isEmpty()
                                       ? QStringLiteral("RTSP Media2")
                                       : QStringLiteral("RTSP Media2 / %1").arg(audioHint);
            StreamOptionEntry e;
            e.id = QStringLiteral("rtsp2::%1").arg(p.token);
            e.label = formatEntryLabel(p, suffix);
            e.profileToken = p.token;
            e.profileName = profName;
            e.uri = p.media2RtspUri.trimmed();
            e.snapshotUri = p.snapshotUri.trimmed();
            entries.push_back(std::move(e));
        }

        const QString trimmedHttp = p.httpUri.trimmed();
        if (!trimmedHttp.isEmpty() &&
            (trimmedHttp.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
             trimmedHttp.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))) {
            StreamOptionEntry e;
            e.id = QStringLiteral("http::%1").arg(p.token);
            e.label = formatEntryLabel(p, QStringLiteral("HTTP MJPEG"));
            e.profileToken = p.token;
            e.profileName = profName;
            e.uri = trimmedHttp;
            e.snapshotUri = p.snapshotUri.trimmed();
            e.suggestedTransport = QStringLiteral("http_mjpeg");
            e.suggestedInputFormat = QStringLiteral("mpjpeg");
            entries.push_back(std::move(e));
        }

        if (!p.snapshotUri.trimmed().isEmpty()) {
            StreamOptionEntry e;
            e.id = QStringLiteral("snapshot::%1").arg(p.token);
            e.label = formatEntryLabel(p, QStringLiteral("Snapshot HTTP JPEG"));
            e.profileToken = p.token;
            e.profileName = profName;
            e.uri = p.rtspUri.trimmed();
            e.snapshotUri = p.snapshotUri.trimmed();
            e.suggestedTransport = QStringLiteral("http_snapshot");
            entries.push_back(std::move(e));
        }
    }
    return entries;
}

QString findMatchingStreamOptionId(const QVector<StreamOptionEntry> &entries, const SourceConfig &cfg)
{
    const QString transport = cfg.rtspTransport.trimmed().toLower();
    const QString token = cfg.profileToken.trimmed();
    const QString uri = cfg.rtspUri.trimmed();
    const QString snap = cfg.snapshotUri.trimmed();

    if (transport == QLatin1String("http_snapshot")) {
        for (const StreamOptionEntry &e : entries) {
            if (e.id.startsWith(QLatin1String("snapshot::")) &&
                (!token.isEmpty() && e.profileToken == token)) {
                return e.id;
            }
        }
        for (const StreamOptionEntry &e : entries) {
            if (e.id.startsWith(QLatin1String("snapshot::")) &&
                (!snap.isEmpty() && e.snapshotUri == snap)) {
                return e.id;
            }
        }
        return QStringLiteral("__custom__");
    }

    if (!uri.isEmpty()) {
        for (const StreamOptionEntry &e : entries) {
            if (e.id.startsWith(QLatin1String("http::")) && e.uri == uri) {
                return e.id;
            }
        }
        for (const StreamOptionEntry &e : entries) {
            if ((e.id.startsWith(QLatin1String("rtsp::")) ||
                 e.id.startsWith(QLatin1String("rtsp2::"))) &&
                e.uri == uri && (token.isEmpty() || e.profileToken == token)) {
                return e.id;
            }
        }
        for (const StreamOptionEntry &e : entries) {
            if ((e.id.startsWith(QLatin1String("rtsp::")) ||
                 e.id.startsWith(QLatin1String("rtsp2::"))) &&
                e.uri == uri) {
                return e.id;
            }
        }
    }

    return QStringLiteral("__custom__");
}

ResolvedStreamParams buildResolvedStreamParams(const SourceConfig &cfg)
{
    ResolvedStreamParams out;

    QString latencyMode = cfg.latencyMode.trimmed().toLower();
    if (latencyMode.isEmpty()) {
        latencyMode = cfg.lowLatency ? QStringLiteral("ultra") : QStringLiteral("standard");
    }

    const QString transport = cfg.rtspTransport.trimmed().toLower();
    if (transport == QLatin1String("http_snapshot")) {
        out.useHttpSnapshot = true;
        out.effectiveUri =
            !cfg.snapshotUri.trimmed().isEmpty() ? cfg.snapshotUri.trimmed() : cfg.rtspUri.trimmed();
        out.asyncUnbuffered = true;
        out.asyncDecoupled = true;
        return out;
    }

    out.effectiveUri =
        OnvifAuth::injectRtspCredentials(cfg.rtspUri.trimmed(), cfg.username, cfg.password);
    out.inputFormat = cfg.inputFormat.trimmed();

    if (out.effectiveUri.isEmpty()) {
        return out;
    }

    const bool isRtsp =
        out.effectiveUri.startsWith(QLatin1String("rtsp://"), Qt::CaseInsensitive) ||
        out.effectiveUri.startsWith(QLatin1String("rtsps://"), Qt::CaseInsensitive);
    const bool isHttp =
        out.effectiveUri.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
        out.effectiveUri.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);

    QStringList opts;
    if (isRtsp) {
        if (transport == QLatin1String("udp")) {
            opts << QStringLiteral("rtsp_transport=udp");
        } else if (transport == QLatin1String("tcp")) {
            opts << QStringLiteral("rtsp_transport=tcp");
        } else if (transport == QLatin1String("udp_multicast")) {
            opts << QStringLiteral("rtsp_transport=udp_multicast");
        } else if (transport == QLatin1String("http")) {
            opts << QStringLiteral("rtsp_transport=http");
        }
        opts << QStringLiteral("timeout=5000000");
        if (!cfg.enableAudio) {
            opts << QStringLiteral("allowed_media_types=video");
        }
    } else if (isHttp) {
        opts << QStringLiteral("reconnect=1")
             << QStringLiteral("reconnect_streamed=1")
             << QStringLiteral("reconnect_delay_max=2");
    }

    if (latencyMode == QLatin1String("zero")) {
        opts << QStringLiteral("fflags=nobuffer+discardcorrupt")
             << QStringLiteral("flags=low_delay")
             << QStringLiteral("max_delay=0")
             << QStringLiteral("reorder_queue_size=0")
             << QStringLiteral("flush_packets=1")
             << QStringLiteral("analyzeduration=0")
             << QStringLiteral("probesize=32768");
        out.bufferingMb = 0;
        out.asyncUnbuffered = true;
        out.asyncDecoupled = true;
    } else if (latencyMode == QLatin1String("ultra")) {
        opts << QStringLiteral("fflags=nobuffer+discardcorrupt")
             << QStringLiteral("flags=low_delay")
             << QStringLiteral("max_delay=0")
             << QStringLiteral("reorder_queue_size=0")
             << QStringLiteral("flush_packets=1")
             << QStringLiteral("analyzeduration=50000")
             << QStringLiteral("probesize=65536");
        out.bufferingMb = 0;
        out.asyncUnbuffered = true;
        out.asyncDecoupled = true;
    } else if (latencyMode == QLatin1String("balanced")) {
        opts << QStringLiteral("fflags=nobuffer")
             << QStringLiteral("flags=low_delay")
             << QStringLiteral("max_delay=20000")
             << QStringLiteral("reorder_queue_size=0")
             << QStringLiteral("analyzeduration=150000")
             << QStringLiteral("probesize=131072");
        out.bufferingMb = std::clamp(cfg.bufferingMb, 0, 8);
        out.asyncUnbuffered = true;
        out.asyncDecoupled = true;
    } else {
        out.bufferingMb = std::clamp(cfg.bufferingMb, 0, 8);
        out.asyncUnbuffered = false;
        out.asyncDecoupled = false;
    }

    const QString extra = cfg.extraFfmpegOptions.trimmed();
    if (!extra.isEmpty()) {
        opts << extra;
    }

    out.ffmpegOptions = opts.join(QLatin1Char(' '));
    return out;
}

} // namespace netdev
