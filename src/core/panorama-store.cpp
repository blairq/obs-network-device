#include "panorama-store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace netdev {

namespace {

constexpr int kThumbMaxW = 320;
constexpr int kThumbMaxH = 180;

QString safeKey(const QString &key)
{
    const QString cleaned = sanitizeCameraKeyPart(key);
    return cleaned.isEmpty() ? QStringLiteral("default_camera") : cleaned;
}

QJsonObject tileToJson(const PanoramaTileMeta &t)
{
    QJsonObject o;
    o.insert(QStringLiteral("col"), t.col);
    o.insert(QStringLiteral("row"), t.row);
    o.insert(QStringLiteral("pan"), t.pan);
    o.insert(QStringLiteral("tilt"), t.tilt);
    o.insert(QStringLiteral("centerX"), t.centerX);
    o.insert(QStringLiteral("centerY"), t.centerY);
    o.insert(QStringLiteral("tileW"), t.tileW);
    o.insert(QStringLiteral("tileH"), t.tileH);
    return o;
}

PanoramaTileMeta tileFromJson(const QJsonObject &o)
{
    PanoramaTileMeta t;
    t.col = o.value(QStringLiteral("col")).toInt();
    t.row = o.value(QStringLiteral("row")).toInt();
    t.pan = o.value(QStringLiteral("pan")).toDouble();
    t.tilt = o.value(QStringLiteral("tilt")).toDouble();
    t.centerX = o.value(QStringLiteral("centerX")).toDouble();
    t.centerY = o.value(QStringLiteral("centerY")).toDouble();
    t.tileW = o.value(QStringLiteral("tileW")).toDouble();
    t.tileH = o.value(QStringLiteral("tileH")).toDouble();
    return t;
}

QJsonObject panoramaMetaToJson(const PanoramaMeta &m)
{
    QJsonObject o;
    o.insert(QStringLiteral("imageFile"), m.imageFile);
    o.insert(QStringLiteral("width"), m.width);
    o.insert(QStringLiteral("height"), m.height);
    o.insert(QStringLiteral("panMin"), m.panMin);
    o.insert(QStringLiteral("panMax"), m.panMax);
    o.insert(QStringLiteral("tiltMin"), m.tiltMin);
    o.insert(QStringLiteral("tiltMax"), m.tiltMax);
    o.insert(QStringLiteral("scanZoom"), m.scanZoom);
    o.insert(QStringLiteral("cols"), m.cols);
    o.insert(QStringLiteral("rows"), m.rows);
    o.insert(QStringLiteral("invertPan"), m.invertPan);
    o.insert(QStringLiteral("invertTilt"), m.invertTilt);
    o.insert(QStringLiteral("capturedAtIso"), m.capturedAtIso);

    QJsonArray arr;
    for (const PanoramaTileMeta &t : m.tiles) {
        arr.append(tileToJson(t));
    }
    o.insert(QStringLiteral("tiles"), arr);
    return o;
}

PanoramaMeta panoramaMetaFromJson(const QJsonObject &o)
{
    PanoramaMeta m;
    m.imageFile = o.value(QStringLiteral("imageFile")).toString();
    m.width = o.value(QStringLiteral("width")).toInt();
    m.height = o.value(QStringLiteral("height")).toInt();
    m.panMin = o.value(QStringLiteral("panMin")).toDouble(-1.0);
    m.panMax = o.value(QStringLiteral("panMax")).toDouble(1.0);
    m.tiltMin = o.value(QStringLiteral("tiltMin")).toDouble(-1.0);
    m.tiltMax = o.value(QStringLiteral("tiltMax")).toDouble(1.0);
    m.scanZoom = o.value(QStringLiteral("scanZoom")).toDouble(0.0);
    m.cols = o.value(QStringLiteral("cols")).toInt();
    m.rows = o.value(QStringLiteral("rows")).toInt();
    m.invertPan = o.value(QStringLiteral("invertPan")).toBool(false);
    m.invertTilt = o.value(QStringLiteral("invertTilt")).toBool(false);
    m.capturedAtIso = o.value(QStringLiteral("capturedAtIso")).toString();

    const QJsonArray arr = o.value(QStringLiteral("tiles")).toArray();
    m.tiles.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (v.isObject()) {
            m.tiles.push_back(tileFromJson(v.toObject()));
        }
    }
    return m;
}

QJsonObject presetToJson(const SavedPtzPreset &p)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), p.id);
    o.insert(QStringLiteral("name"), p.name);
    o.insert(QStringLiteral("pan"), p.pan);
    o.insert(QStringLiteral("tilt"), p.tilt);
    o.insert(QStringLiteral("zoom"), p.zoom);
    o.insert(QStringLiteral("hasPosition"), p.hasPosition);
    o.insert(QStringLiteral("cameraPresetToken"), p.cameraPresetToken);
    o.insert(QStringLiteral("thumbnailFile"), p.thumbnailFile);
    o.insert(QStringLiteral("createdAtIso"), p.createdAtIso);
    return o;
}

SavedPtzPreset presetFromJson(const QJsonObject &o)
{
    SavedPtzPreset p;
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.pan = o.value(QStringLiteral("pan")).toDouble();
    p.tilt = o.value(QStringLiteral("tilt")).toDouble();
    p.zoom = o.value(QStringLiteral("zoom")).toDouble();
    p.hasPosition = o.value(QStringLiteral("hasPosition")).toBool(false);
    p.cameraPresetToken = o.value(QStringLiteral("cameraPresetToken")).toString();
    p.thumbnailFile = o.value(QStringLiteral("thumbnailFile")).toString();
    p.createdAtIso = o.value(QStringLiteral("createdAtIso")).toString();
    return p;
}

bool readProfileJson(const QString &filePath, CameraPtzProfile &out)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject o = doc.object();
    out.cameraKey = o.value(QStringLiteral("cameraKey")).toString();
    out.cameraName = o.value(QStringLiteral("cameraName")).toString();
    out.manufacturer = o.value(QStringLiteral("manufacturer")).toString();
    out.model = o.value(QStringLiteral("model")).toString();
    out.serialNumber = o.value(QStringLiteral("serialNumber")).toString();
    out.deviceServiceUrl = o.value(QStringLiteral("deviceServiceUrl")).toString();
    out.ptzServiceUrl = o.value(QStringLiteral("ptzServiceUrl")).toString();
    out.invertPan = o.value(QStringLiteral("invertPan")).toBool(false);
    out.invertTilt = o.value(QStringLiteral("invertTilt")).toBool(false);
    if (o.contains(QStringLiteral("panorama")) && o.value(QStringLiteral("panorama")).isObject()) {
        out.panorama = panoramaMetaFromJson(o.value(QStringLiteral("panorama")).toObject());
    } else {
        out.panorama = PanoramaMeta{};
    }

    out.savedPresets.clear();
    const QJsonArray arr = o.value(QStringLiteral("savedPresets")).toArray();
    for (const QJsonValue &v : arr) {
        if (v.isObject()) {
            out.savedPresets.push_back(presetFromJson(v.toObject()));
        }
    }
    return true;
}

} // namespace

PanoramaStore::PanoramaStore(QString baseDir) : m_baseDir(std::move(baseDir)) {}

QString PanoramaStore::cameraDir(const QString &cameraKey) const
{
    return QDir(m_baseDir).filePath(safeKey(cameraKey));
}

QString PanoramaStore::resolveExistingCameraDir(const QString &cameraKey,
                                                const QString &fallbackHostKey) const
{
    const QString primaryDir = cameraDir(cameraKey);
    if (QFileInfo::exists(QDir(primaryDir).filePath(QStringLiteral("profile.json")))) {
        return primaryDir;
    }
    if (!fallbackHostKey.isEmpty()) {
        const QString fbDir = cameraDir(fallbackHostKey);
        if (QFileInfo::exists(QDir(fbDir).filePath(QStringLiteral("profile.json")))) {
            return fbDir;
        }
    }
    return QString();
}

bool PanoramaStore::loadProfile(const QString &cameraKey,
                                const QString &fallbackHostKey,
                                CameraPtzProfile &profileOut,
                                QImage &panoramaOut) const
{
    profileOut = CameraPtzProfile{};
    panoramaOut = QImage();

    const QString dir = resolveExistingCameraDir(cameraKey, fallbackHostKey);
    if (dir.isEmpty()) {
        profileOut.cameraKey = safeKey(cameraKey);
        return false;
    }

    if (!readProfileJson(QDir(dir).filePath(QStringLiteral("profile.json")), profileOut)) {
        profileOut.cameraKey = safeKey(cameraKey);
        return false;
    }

    if (!profileOut.panorama.imageFile.isEmpty()) {
        const QString imgPath = QDir(dir).filePath(profileOut.panorama.imageFile);
        if (QFileInfo::exists(imgPath)) {
            panoramaOut.load(imgPath);
        }
    }
    return true;
}

bool PanoramaStore::saveProfile(const CameraPtzProfile &profile) const
{
    const QString dirPath = cameraDir(profile.cameraKey);
    if (!QDir().mkpath(dirPath)) {
        return false;
    }

    QJsonObject o;
    o.insert(QStringLiteral("version"), 1);
    o.insert(QStringLiteral("cameraKey"), safeKey(profile.cameraKey));
    o.insert(QStringLiteral("cameraName"), profile.cameraName);
    o.insert(QStringLiteral("manufacturer"), profile.manufacturer);
    o.insert(QStringLiteral("model"), profile.model);
    o.insert(QStringLiteral("serialNumber"), profile.serialNumber);
    o.insert(QStringLiteral("deviceServiceUrl"), profile.deviceServiceUrl);
    o.insert(QStringLiteral("ptzServiceUrl"), profile.ptzServiceUrl);
    o.insert(QStringLiteral("invertPan"), profile.invertPan);
    o.insert(QStringLiteral("invertTilt"), profile.invertTilt);
    if (profile.panorama.isValid()) {
        o.insert(QStringLiteral("panorama"), panoramaMetaToJson(profile.panorama));
    }

    QJsonArray presetsArr;
    for (const SavedPtzPreset &p : profile.savedPresets) {
        presetsArr.append(presetToJson(p));
    }
    o.insert(QStringLiteral("savedPresets"), presetsArr);

    QSaveFile file(QDir(dirPath).filePath(QStringLiteral("profile.json")));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return file.commit();
}

bool PanoramaStore::savePanorama(CameraPtzProfile &profile,
                                 const QImage &image,
                                 const PanoramaMeta &meta) const
{
    if (image.isNull() || !meta.isValid()) {
        return false;
    }

    const QString dirPath = cameraDir(profile.cameraKey);
    if (!QDir().mkpath(dirPath)) {
        return false;
    }

    const QString relImage = meta.imageFile.isEmpty() ? QStringLiteral("panorama.jpg") : meta.imageFile;
    const QString absImage = QDir(dirPath).filePath(relImage);
    if (!image.save(absImage, "JPG", 90)) {
        return false;
    }

    profile.panorama = meta;
    profile.panorama.imageFile = relImage;
    return saveProfile(profile);
}

QString PanoramaStore::savePresetThumbnail(const QString &cameraKey,
                                           const QString &presetId,
                                           const QImage &image) const
{
    if (image.isNull() || presetId.trimmed().isEmpty()) {
        return QString();
    }

    const QString dirPath = cameraDir(cameraKey);
    const QString presetsDirPath = QDir(dirPath).filePath(QStringLiteral("presets"));
    if (!QDir().mkpath(presetsDirPath)) {
        return QString();
    }

    const QString cleanId = safeKey(presetId);
    const QString relPath = QStringLiteral("presets/%1.jpg").arg(cleanId);
    const QImage thumb =
        image.scaled(kThumbMaxW, kThumbMaxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!thumb.save(QDir(dirPath).filePath(relPath), "JPG", 88)) {
        return QString();
    }
    return relPath;
}

QImage PanoramaStore::loadPresetThumbnail(const QString &cameraKey,
                                          const QString &relativePath) const
{
    if (relativePath.trimmed().isEmpty()) {
        return QImage();
    }
    const QString absPath = QDir(cameraDir(cameraKey)).filePath(relativePath);
    if (!QFileInfo::exists(absPath)) {
        return QImage();
    }
    return QImage(absPath);
}

void PanoramaStore::removePresetThumbnail(const QString &cameraKey,
                                          const QString &relativePath) const
{
    if (relativePath.trimmed().isEmpty()) {
        return;
    }
    const QString absPath = QDir(cameraDir(cameraKey)).filePath(relativePath);
    if (QFileInfo::exists(absPath)) {
        QFile::remove(absPath);
    }
}

} // namespace netdev
