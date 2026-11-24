#include "panorama-builder.hpp"

#include <QColor>
#include <QDateTime>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <vector>

namespace netdev {

namespace {

constexpr int kBaseTileWidth = 480;
constexpr int kMaxPanoramaWidth = 4096;
constexpr int kRegWidth = 160;
constexpr int kMaxRefineShiftPx = 12;
constexpr double kDefaultStepRatio = 0.72;
constexpr double kMinStepRatio = 0.06;
constexpr double kMaxStepRatio = 0.95;

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
    double stdDev = 0.0;
};

GrayImage makeGrayImage(const QImage &src, int targetW)
{
    GrayImage out;
    if (src.isNull() || src.width() <= 0 || src.height() <= 0 || targetW <= 0) {
        return out;
    }
    const QImage gray = src.scaledToWidth(targetW, Qt::FastTransformation)
                            .convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull() || gray.width() <= 0 || gray.height() <= 0) {
        return out;
    }

    out.width = gray.width();
    out.height = gray.height();
    out.data.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height));

    double sum = 0.0;
    double sumSq = 0.0;
    for (int y = 0; y < out.height; ++y) {
        const uchar *line = gray.constScanLine(y);
        for (int x = 0; x < out.width; ++x) {
            const uint8_t v = line[x];
            out.data[static_cast<size_t>(y) * static_cast<size_t>(out.width) + static_cast<size_t>(x)] = v;
            const double dv = static_cast<double>(v);
            sum += dv;
            sumSq += dv * dv;
        }
    }

    const double n = static_cast<double>(out.data.size());
    if (n > 1.0) {
        const double mean = sum / n;
        const double var = std::max(0.0, (sumSq / n) - (mean * mean));
        out.stdDev = std::sqrt(var);
    }
    return out;
}

struct PairMatch {
    bool confident = false;
    double stepFull = 0.0;
    double crossShiftFull = 0.0;
};

PairMatch estimateHorizontalPair(const GrayImage &left, const GrayImage &right, int fullTileW)
{
    PairMatch res;
    if (left.width < 24 || right.width < 24 || left.height != right.height ||
        left.stdDev < 5.0 || right.stdDev < 5.0) {
        return res;
    }

    const int w = left.width;
    const int h = left.height;
    const int minOv = std::max(8, static_cast<int>(std::round(w * (1.0 - kMaxStepRatio))));
    const int maxOv = std::min(w - 4, static_cast<int>(std::round(w * (1.0 - kMinStepRatio))));
    if (minOv >= maxOv) {
        return res;
    }

    const int maxDy = std::clamp(h / 12, 3, 10);
    double bestScore = 1e18;
    int bestOv = (minOv + maxOv) / 2;
    int bestDy = 0;

    for (int ov = minOv; ov <= maxOv; ++ov) {
        for (int dy = -maxDy; dy <= maxDy; ++dy) {
            const int yStart = std::max(0, dy);
            const int yEnd = std::min(h, h + dy);
            const int validH = yEnd - yStart;
            if (validH < (h * 2) / 3) {
                continue;
            }

            double diffSum = 0.0;
            const int leftX0 = w - ov;
            for (int yL = yStart; yL < yEnd; ++yL) {
                const int yR = yL - dy;
                const uint8_t *rowL = &left.data[static_cast<size_t>(yL) * static_cast<size_t>(w) + static_cast<size_t>(leftX0)];
                const uint8_t *rowR = &right.data[static_cast<size_t>(yR) * static_cast<size_t>(w)];
                for (int x = 0; x < ov; ++x) {
                    diffSum += std::abs(static_cast<int>(rowL[x]) - static_cast<int>(rowR[x]));
                }
            }

            const double count = static_cast<double>(ov) * static_cast<double>(validH);
            const double mad = diffSum / count;
            const double score = mad + 0.15 * static_cast<double>(std::abs(dy));
            if (score < bestScore) {
                bestScore = score;
                bestOv = ov;
                bestDy = dy;
            }
        }
    }

    if (bestScore < 42.0) {
        const double scale = static_cast<double>(fullTileW) / static_cast<double>(w);
        res.confident = true;
        res.stepFull = static_cast<double>(w - bestOv) * scale;
        res.crossShiftFull = static_cast<double>(bestDy) * scale;
    }
    return res;
}

PairMatch estimateVerticalPair(const GrayImage &top, const GrayImage &bottom, int fullTileH)
{
    PairMatch res;
    if (top.height < 24 || bottom.height < 24 || top.width != bottom.width ||
        top.stdDev < 5.0 || bottom.stdDev < 5.0) {
        return res;
    }

    const int w = top.width;
    const int h = top.height;
    const int minOv = std::max(8, static_cast<int>(std::round(h * (1.0 - kMaxStepRatio))));
    const int maxOv = std::min(h - 4, static_cast<int>(std::round(h * (1.0 - kMinStepRatio))));
    if (minOv >= maxOv) {
        return res;
    }

    const int maxDx = std::clamp(w / 12, 3, 10);
    double bestScore = 1e18;
    int bestOv = (minOv + maxOv) / 2;
    int bestDx = 0;

    for (int ov = minOv; ov <= maxOv; ++ov) {
        for (int dx = -maxDx; dx <= maxDx; ++dx) {
            const int xStart = std::max(0, dx);
            const int xEnd = std::min(w, w + dx);
            const int validW = xEnd - xStart;
            if (validW < (w * 2) / 3) {
                continue;
            }

            double diffSum = 0.0;
            const int topY0 = h - ov;
            for (int y = 0; y < ov; ++y) {
                const uint8_t *rowT = &top.data[static_cast<size_t>(topY0 + y) * static_cast<size_t>(w)];
                const uint8_t *rowB = &bottom.data[static_cast<size_t>(y) * static_cast<size_t>(w)];
                for (int xT = xStart; xT < xEnd; ++xT) {
                    const int xB = xT - dx;
                    diffSum += std::abs(static_cast<int>(rowT[xT]) - static_cast<int>(rowB[xB]));
                }
            }

            const double count = static_cast<double>(ov) * static_cast<double>(validW);
            const double mad = diffSum / count;
            const double score = mad + 0.15 * static_cast<double>(std::abs(dx));
            if (score < bestScore) {
                bestScore = score;
                bestOv = ov;
                bestDx = dx;
            }
        }
    }

    if (bestScore < 42.0) {
        const double scale = static_cast<double>(fullTileH) / static_cast<double>(h);
        res.confident = true;
        res.stepFull = static_cast<double>(h - bestOv) * scale;
        res.crossShiftFull = static_cast<double>(bestDx) * scale;
    }
    return res;
}

double medianValue(std::vector<double> vals, double fallback)
{
    if (vals.empty()) {
        return fallback;
    }
    std::sort(vals.begin(), vals.end());
    const size_t n = vals.size();
    if ((n % 2) == 1) {
        return vals[n / 2];
    }
    return 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
}

std::vector<float> makeFeatherWeights1D(int length)
{
    std::vector<float> w(static_cast<size_t>(std::max(1, length)), 1.0f);
    if (length <= 2) {
        return w;
    }
    const float ramp = std::max(4.0f, 0.28f * static_cast<float>(length));
    for (int i = 0; i < length; ++i) {
        const float distLeft = static_cast<float>(i) + 0.5f;
        const float distRight = static_cast<float>(length - 1 - i) + 0.5f;
        const float dist = std::min(distLeft, distRight);
        if (dist < ramp) {
            const float t = std::clamp(dist / ramp, 0.02f, 1.0f);
            const float s = std::sin(1.57079632679f * t);
            w[static_cast<size_t>(i)] = s * s;
        }
    }
    return w;
}

bool buildLookupGrid(const PanoramaMeta &meta, QVector<const PanoramaTileMeta *> &gridOut)
{
    if (!meta.isValid()) {
        return false;
    }
    const int total = meta.cols * meta.rows;
    gridOut.fill(nullptr, total);
    for (const PanoramaTileMeta &t : meta.tiles) {
        if (t.col >= 0 && t.col < meta.cols && t.row >= 0 && t.row < meta.rows) {
            gridOut[t.row * meta.cols + t.col] = &t;
        }
    }
    for (int i = 0; i < total; ++i) {
        if (!gridOut[i]) {
            return false;
        }
    }
    return true;
}

double lerp1(double a, double b, double t)
{
    return a + (b - a) * t;
}

double bilerp(double v00, double v10, double v01, double v11, double u, double v)
{
    const double top = lerp1(v00, v10, u);
    const double bot = lerp1(v01, v11, u);
    return lerp1(top, bot, v);
}

int findMonotonicBracket(const QVector<double> &axis, double val, bool ascending)
{
    const int n = static_cast<int>(axis.size());
    if (n <= 2) {
        return 0;
    }
    if (ascending) {
        for (int i = 0; i < n - 2; ++i) {
            if (val <= axis[i + 1]) {
                return i;
            }
        }
        return n - 2;
    }
    for (int i = 0; i < n - 2; ++i) {
        if (val >= axis[i + 1]) {
            return i;
        }
    }
    return n - 2;
}

double safeFraction(double val, double a, double b, double minClamp, double maxClamp)
{
    const double d = b - a;
    if (std::abs(d) < 1e-9) {
        return 0.0;
    }
    return std::clamp((val - a) / d, minClamp, maxClamp);
}

} // namespace

PanoramaBuildResult PanoramaBuilder::build(const QVector<CapturedTile> &tiles,
                                         const PanoramaScanSettings &settings)
{
    PanoramaBuildResult out;
    const int cols = std::max(1, settings.cols);
    const int rows = std::max(1, settings.rows);

    struct Slot {
        bool valid = false;
        double pan = 0.0;
        double tilt = 0.0;
        QImage image;
        GrayImage gray;
        double x = 0.0;
        double y = 0.0;
    };

    QVector<Slot> grid(cols * rows);
    int validCount = 0;
    int commonTileH = 0;

    for (const CapturedTile &t : tiles) {
        if (t.col < 0 || t.col >= cols || t.row < 0 || t.row >= rows) {
            continue;
        }
        if (t.image.isNull() || t.image.width() <= 0 || t.image.height() <= 0) {
            continue;
        }
        QImage scaled = t.image.scaledToWidth(kBaseTileWidth, Qt::SmoothTransformation)
                            .convertToFormat(QImage::Format_RGB32);
        if (scaled.isNull() || scaled.height() <= 0) {
            continue;
        }
        if (commonTileH == 0) {
            commonTileH = scaled.height();
        } else if (scaled.height() != commonTileH) {
            scaled = scaled.scaled(kBaseTileWidth, commonTileH, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
        }

        Slot &s = grid[t.row * cols + t.col];
        if (!s.valid) {
            ++validCount;
        }
        s.valid = true;
        s.pan = t.pan;
        s.tilt = t.tilt;
        s.image = scaled;
        s.gray = makeGrayImage(scaled, kRegWidth);
    }

    if (validCount == 0 || commonTileH <= 0) {
        out.error = QStringLiteral("No se capturaron imagenes validas para armar la panoramica.");
        return out;
    }

    const int tileW = kBaseTileWidth;
    const int tileH = commonTileH;

    // Fill nominal pan/tilt for any missing slots so metadata stays rectangular
    for (int r = 0; r < rows; ++r) {
        const double rowFrac = (rows > 1) ? static_cast<double>(r) / static_cast<double>(rows - 1) : 0.5;
        const double nomTilt = settings.invertTilt
                                   ? (settings.tiltMin + rowFrac * (settings.tiltMax - settings.tiltMin))
                                   : (settings.tiltMax - rowFrac * (settings.tiltMax - settings.tiltMin));
        for (int c = 0; c < cols; ++c) {
            const double colFrac = (cols > 1) ? static_cast<double>(c) / static_cast<double>(cols - 1) : 0.5;
            const double nomPan = settings.invertPan
                                      ? (settings.panMax - colFrac * (settings.panMax - settings.panMin))
                                      : (settings.panMin + colFrac * (settings.panMax - settings.panMin));
            Slot &s = grid[r * cols + c];
            if (!s.valid) {
                s.pan = nomPan;
                s.tilt = nomTilt;
            }
        }
    }

    // Estimate median horizontal and vertical step sizes from overlapping neighbor pairs
    std::vector<double> hSteps;
    std::vector<PairMatch> hMatches(static_cast<size_t>(cols * rows));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c + 1 < cols; ++c) {
            const Slot &a = grid[r * cols + c];
            const Slot &b = grid[r * cols + (c + 1)];
            if (a.valid && b.valid) {
                const PairMatch m = estimateHorizontalPair(a.gray, b.gray, tileW);
                hMatches[static_cast<size_t>(r * cols + c)] = m;
                if (m.confident) {
                    hSteps.push_back(m.stepFull);
                }
            }
        }
    }

    std::vector<double> vSteps;
    std::vector<PairMatch> vMatches(static_cast<size_t>(cols * rows));
    for (int r = 0; r + 1 < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const Slot &a = grid[r * cols + c];
            const Slot &b = grid[(r + 1) * cols + c];
            if (a.valid && b.valid) {
                const PairMatch m = estimateVerticalPair(a.gray, b.gray, tileH);
                vMatches[static_cast<size_t>(r * cols + c)] = m;
                if (m.confident) {
                    vSteps.push_back(m.stepFull);
                }
            }
        }
    }

    const double stepX = std::clamp(
        medianValue(hSteps, tileW * kDefaultStepRatio),
        tileW * kMinStepRatio, tileW * kMaxStepRatio);
    const double stepY = std::clamp(
        medianValue(vSteps, tileH * kDefaultStepRatio),
        tileH * kMinStepRatio, tileH * kMaxStepRatio);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            Slot &s = grid[r * cols + c];
            s.x = static_cast<double>(c) * stepX;
            s.y = static_cast<double>(r) * stepY;
        }
    }

    // Gentle local refinement constrained within +-kMaxRefineShiftPx to preserve monotonic ordering
    for (int iter = 0; iter < 2; ++iter) {
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const double baseX = static_cast<double>(c) * stepX;
                const double baseY = static_cast<double>(r) * stepY;
                double targetX = 0.0;
                double targetY = 0.0;
                int weightSum = 0;

                if (c > 0 && hMatches[static_cast<size_t>(r * cols + (c - 1))].confident) {
                    const Slot &left = grid[r * cols + (c - 1)];
                    const PairMatch &m = hMatches[static_cast<size_t>(r * cols + (c - 1))];
                    targetX += left.x + m.stepFull;
                    targetY += left.y + m.crossShiftFull;
                    ++weightSum;
                }
                if (c + 1 < cols && hMatches[static_cast<size_t>(r * cols + c)].confident) {
                    const Slot &right = grid[r * cols + (c + 1)];
                    const PairMatch &m = hMatches[static_cast<size_t>(r * cols + c)];
                    targetX += right.x - m.stepFull;
                    targetY += right.y - m.crossShiftFull;
                    ++weightSum;
                }
                if (r > 0 && vMatches[static_cast<size_t>((r - 1) * cols + c)].confident) {
                    const Slot &top = grid[(r - 1) * cols + c];
                    const PairMatch &m = vMatches[static_cast<size_t>((r - 1) * cols + c)];
                    targetX += top.x + m.crossShiftFull;
                    targetY += top.y + m.stepFull;
                    ++weightSum;
                }
                if (r + 1 < rows && vMatches[static_cast<size_t>(r * cols + c)].confident) {
                    const Slot &bot = grid[(r + 1) * cols + c];
                    const PairMatch &m = vMatches[static_cast<size_t>(r * cols + c)];
                    targetX += bot.x - m.crossShiftFull;
                    targetY += bot.y - m.stepFull;
                    ++weightSum;
                }

                if (weightSum > 0) {
                    const double avgX = targetX / static_cast<double>(weightSum);
                    const double avgY = targetY / static_cast<double>(weightSum);
                    grid[r * cols + c].x = std::clamp(
                        avgX, baseX - kMaxRefineShiftPx, baseX + kMaxRefineShiftPx);
                    grid[r * cols + c].y = std::clamp(
                        avgY, baseY - kMaxRefineShiftPx, baseY + kMaxRefineShiftPx);
                }
            }
        }
    }

    double minX = 1e18;
    double minY = 1e18;
    double maxX = -1e18;
    double maxY = -1e18;
    for (const Slot &s : grid) {
        minX = std::min(minX, s.x);
        minY = std::min(minY, s.y);
        maxX = std::max(maxX, s.x);
        maxY = std::max(maxY, s.y);
    }

    for (Slot &s : grid) {
        s.x -= minX;
        s.y -= minY;
    }

    const double rawCanvasW = (maxX - minX) + static_cast<double>(tileW);
    const double rawCanvasH = (maxY - minY) + static_cast<double>(tileH);
    const double outScale =
        (rawCanvasW > static_cast<double>(kMaxPanoramaWidth))
            ? (static_cast<double>(kMaxPanoramaWidth) / rawCanvasW)
            : 1.0;

    const int canvasW = std::max(1, static_cast<int>(std::ceil(rawCanvasW * outScale)));
    const int canvasH = std::max(1, static_cast<int>(std::ceil(rawCanvasH * outScale)));
    const int scaledTileW = std::max(1, static_cast<int>(std::round(tileW * outScale)));
    const int scaledTileH = std::max(1, static_cast<int>(std::round(tileH * outScale)));

    const std::vector<float> wx = makeFeatherWeights1D(scaledTileW);
    const std::vector<float> wy = makeFeatherWeights1D(scaledTileH);

    const size_t totalPx = static_cast<size_t>(canvasW) * static_cast<size_t>(canvasH);
    std::vector<float> accumR(totalPx, 0.0f);
    std::vector<float> accumG(totalPx, 0.0f);
    std::vector<float> accumB(totalPx, 0.0f);
    std::vector<float> accumW(totalPx, 0.0f);

    double actualMinPan = std::min(settings.panMin, settings.panMax);
    double actualMaxPan = std::max(settings.panMin, settings.panMax);
    double actualMinTilt = std::min(settings.tiltMin, settings.tiltMax);
    double actualMaxTilt = std::max(settings.tiltMin, settings.tiltMax);

    out.meta = PanoramaMeta{};
    out.meta.imageFile = QStringLiteral("panorama.jpg");
    out.meta.width = canvasW;
    out.meta.height = canvasH;
    out.meta.cols = cols;
    out.meta.rows = rows;
    out.meta.scanZoom = settings.scanZoom;
    out.meta.invertPan = settings.invertPan;
    out.meta.invertTilt = settings.invertTilt;
    out.meta.capturedAtIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    out.meta.tiles.reserve(cols * rows);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const Slot &s = grid[r * cols + c];
            const double tileX = s.x * outScale;
            const double tileY = s.y * outScale;

            actualMinPan = std::min(actualMinPan, s.pan);
            actualMaxPan = std::max(actualMaxPan, s.pan);
            actualMinTilt = std::min(actualMinTilt, s.tilt);
            actualMaxTilt = std::max(actualMaxTilt, s.tilt);

            PanoramaTileMeta metaTile;
            metaTile.col = c;
            metaTile.row = r;
            metaTile.pan = s.pan;
            metaTile.tilt = s.tilt;
            metaTile.tileW = static_cast<double>(scaledTileW);
            metaTile.tileH = static_cast<double>(scaledTileH);
            metaTile.centerX = tileX + 0.5 * metaTile.tileW;
            metaTile.centerY = tileY + 0.5 * metaTile.tileH;
            out.meta.tiles.push_back(metaTile);

            if (!s.valid) {
                continue;
            }

            const QImage blendImg =
                (scaledTileW == tileW && scaledTileH == tileH)
                    ? s.image
                    : s.image.scaled(scaledTileW, scaledTileH, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);

            const int dstX0 = static_cast<int>(std::round(tileX));
            const int dstY0 = static_cast<int>(std::round(tileY));

            for (int sy = 0; sy < scaledTileH; ++sy) {
                const int dy = dstY0 + sy;
                if (dy < 0 || dy >= canvasH) {
                    continue;
                }
                const float yWeight = wy[static_cast<size_t>(sy)];
                const QRgb *srcLine = reinterpret_cast<const QRgb *>(blendImg.constScanLine(sy));
                const size_t rowOffset = static_cast<size_t>(dy) * static_cast<size_t>(canvasW);

                for (int sx = 0; sx < scaledTileW; ++sx) {
                    const int dx = dstX0 + sx;
                    if (dx < 0 || dx >= canvasW) {
                        continue;
                    }
                    const float weight = wx[static_cast<size_t>(sx)] * yWeight;
                    const QRgb px = srcLine[sx];
                    const size_t idx = rowOffset + static_cast<size_t>(dx);
                    accumR[idx] += static_cast<float>(qRed(px)) * weight;
                    accumG[idx] += static_cast<float>(qGreen(px)) * weight;
                    accumB[idx] += static_cast<float>(qBlue(px)) * weight;
                    accumW[idx] += weight;
                }
            }
        }
    }

    out.meta.panMin = actualMinPan;
    out.meta.panMax = actualMaxPan;
    out.meta.tiltMin = actualMinTilt;
    out.meta.tiltMax = actualMaxTilt;

    QImage stitched(canvasW, canvasH, QImage::Format_RGB32);
    stitched.fill(QColor(18, 20, 26));

    for (int y = 0; y < canvasH; ++y) {
        QRgb *dstLine = reinterpret_cast<QRgb *>(stitched.scanLine(y));
        const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(canvasW);
        for (int x = 0; x < canvasW; ++x) {
            const size_t idx = rowOffset + static_cast<size_t>(x);
            const float w = accumW[idx];
            if (w > 1e-5f) {
                const int r = std::clamp(static_cast<int>(std::lround(accumR[idx] / w)), 0, 255);
                const int g = std::clamp(static_cast<int>(std::lround(accumG[idx] / w)), 0, 255);
                const int b = std::clamp(static_cast<int>(std::lround(accumB[idx] / w)), 0, 255);
                dstLine[x] = qRgb(r, g, b);
            }
        }
    }

    out.image = std::move(stitched);
    out.ok = true;
    return out;
}

bool PanoramaMapper::pixelToPanTilt(const PanoramaMeta &meta,
                                    double px,
                                    double py,
                                    double &panOut,
                                    double &tiltOut)
{
    QVector<const PanoramaTileMeta *> grid;
    if (!buildLookupGrid(meta, grid)) {
        return false;
    }

    const int cols = meta.cols;
    const int rows = meta.rows;
    const double clampedPx = std::clamp(px, 0.0, static_cast<double>(meta.width));
    const double clampedPy = std::clamp(py, 0.0, static_cast<double>(meta.height));

    QVector<double> colCenterX(cols, 0.0);
    for (int c = 0; c < cols; ++c) {
        double sum = 0.0;
        for (int r = 0; r < rows; ++r) {
            sum += grid[r * cols + c]->centerX;
        }
        colCenterX[c] = sum / static_cast<double>(rows);
    }

    QVector<double> rowCenterY(rows, 0.0);
    for (int r = 0; r < rows; ++r) {
        double sum = 0.0;
        for (int c = 0; c < cols; ++c) {
            sum += grid[r * cols + c]->centerY;
        }
        rowCenterY[r] = sum / static_cast<double>(cols);
    }

    const double minPan = std::min(meta.panMin, meta.panMax);
    const double maxPan = std::max(meta.panMin, meta.panMax);
    const double minTilt = std::min(meta.tiltMin, meta.tiltMax);
    const double maxTilt = std::max(meta.tiltMin, meta.tiltMax);
    const double limitPanMin = std::min(-1.0, minPan);
    const double limitPanMax = std::max(1.0, maxPan);
    const double limitTiltMin = std::min(-1.0, minTilt);
    const double limitTiltMax = std::max(1.0, maxTilt);

    if (cols == 1 && rows == 1) {
        const double fx = clampedPx / static_cast<double>(std::max(1, meta.width));
        const double fy = clampedPy / static_cast<double>(std::max(1, meta.height));
        const double leftPan = meta.invertPan ? maxPan : minPan;
        const double rightPan = meta.invertPan ? minPan : maxPan;
        const double topTilt = meta.invertTilt ? minTilt : maxTilt;
        const double bottomTilt = meta.invertTilt ? maxTilt : minTilt;
        panOut = (std::abs(rightPan - leftPan) < 1e-6) ? grid[0]->pan : lerp1(leftPan, rightPan, fx);
        tiltOut = (std::abs(bottomTilt - topTilt) < 1e-6) ? grid[0]->tilt : lerp1(topTilt, bottomTilt, fy);
        return true;
    }

    int c0 = 0;
    int c1 = 0;
    double u = 0.0;
    if (cols >= 2) {
        c0 = findMonotonicBracket(colCenterX, clampedPx, true);
        c1 = c0 + 1;
        u = safeFraction(clampedPx, colCenterX[c0], colCenterX[c1], -6.0, 7.0);
    }

    int r0 = 0;
    int r1 = 0;
    double v = 0.0;
    if (rows >= 2) {
        r0 = findMonotonicBracket(rowCenterY, clampedPy, true);
        r1 = r0 + 1;
        v = safeFraction(clampedPy, rowCenterY[r0], rowCenterY[r1], -6.0, 7.0);
    }

    const PanoramaTileMeta *t00 = grid[r0 * cols + c0];
    const PanoramaTileMeta *t10 = grid[r0 * cols + c1];
    const PanoramaTileMeta *t01 = grid[r1 * cols + c0];
    const PanoramaTileMeta *t11 = grid[r1 * cols + c1];

    for (int iter = 0; iter < 2; ++iter) {
        const double uClamp = std::clamp(u, 0.0, 1.0);
        const double vClamp = std::clamp(v, 0.0, 1.0);
        if (cols >= 2) {
            const double x0 = lerp1(t00->centerX, t01->centerX, vClamp);
            const double x1 = lerp1(t10->centerX, t11->centerX, vClamp);
            u = safeFraction(clampedPx, x0, x1, -6.0, 7.0);
        }
        if (rows >= 2) {
            const double y0 = lerp1(t00->centerY, t10->centerY, uClamp);
            const double y1 = lerp1(t01->centerY, t11->centerY, uClamp);
            v = safeFraction(clampedPy, y0, y1, -6.0, 7.0);
        }
    }

    if (cols == 1) {
        const double fx = clampedPx / static_cast<double>(std::max(1, meta.width));
        const double leftPan = meta.invertPan ? maxPan : minPan;
        const double rightPan = meta.invertPan ? minPan : maxPan;
        panOut = (std::abs(rightPan - leftPan) < 1e-6)
                     ? lerp1(t00->pan, t01->pan, std::clamp(v, 0.0, 1.0))
                     : lerp1(leftPan, rightPan, fx);
    } else {
        panOut = std::clamp(bilerp(t00->pan, t10->pan, t01->pan, t11->pan, u, v),
                            limitPanMin, limitPanMax);
    }

    if (rows == 1) {
        const double fy = clampedPy / static_cast<double>(std::max(1, meta.height));
        const double topTilt = meta.invertTilt ? minTilt : maxTilt;
        const double bottomTilt = meta.invertTilt ? maxTilt : minTilt;
        tiltOut = (std::abs(bottomTilt - topTilt) < 1e-6)
                      ? lerp1(t00->tilt, t10->tilt, std::clamp(u, 0.0, 1.0))
                      : lerp1(topTilt, bottomTilt, fy);
    } else {
        tiltOut = std::clamp(bilerp(t00->tilt, t10->tilt, t01->tilt, t11->tilt, u, v),
                             limitTiltMin, limitTiltMax);
    }

    return true;
}

bool PanoramaMapper::panTiltToPixel(const PanoramaMeta &meta,
                                    double pan,
                                    double tilt,
                                    double &pxOut,
                                    double &pyOut)
{
    QVector<const PanoramaTileMeta *> grid;
    if (!buildLookupGrid(meta, grid)) {
        return false;
    }

    const int cols = meta.cols;
    const int rows = meta.rows;
    const double minPan = std::min(meta.panMin, meta.panMax);
    const double maxPan = std::max(meta.panMin, meta.panMax);
    const double minTilt = std::min(meta.tiltMin, meta.tiltMax);
    const double maxTilt = std::max(meta.tiltMin, meta.tiltMax);
    const double limitPanMin = std::min(-1.0, minPan);
    const double limitPanMax = std::max(1.0, maxPan);
    const double limitTiltMin = std::min(-1.0, minTilt);
    const double limitTiltMax = std::max(1.0, maxTilt);
    const double clampedPan = std::clamp(pan, limitPanMin, limitPanMax);
    const double clampedTilt = std::clamp(tilt, limitTiltMin, limitTiltMax);

    if (cols == 1 && rows == 1) {
        const double leftPan = meta.invertPan ? maxPan : minPan;
        const double rightPan = meta.invertPan ? minPan : maxPan;
        const double topTilt = meta.invertTilt ? minTilt : maxTilt;
        const double bottomTilt = meta.invertTilt ? maxTilt : minTilt;
        const double fx = (std::abs(rightPan - leftPan) < 1e-6)
                              ? 0.5
                              : std::clamp((clampedPan - leftPan) / (rightPan - leftPan), 0.0, 1.0);
        const double fy = (std::abs(bottomTilt - topTilt) < 1e-6)
                              ? 0.5
                              : std::clamp((clampedTilt - topTilt) / (bottomTilt - topTilt), 0.0, 1.0);
        pxOut = fx * static_cast<double>(meta.width);
        pyOut = fy * static_cast<double>(meta.height);
        return true;
    }

    QVector<double> colPan(cols, 0.0);
    for (int c = 0; c < cols; ++c) {
        double sum = 0.0;
        for (int r = 0; r < rows; ++r) {
            sum += grid[r * cols + c]->pan;
        }
        colPan[c] = sum / static_cast<double>(rows);
    }

    QVector<double> rowTilt(rows, 0.0);
    for (int r = 0; r < rows; ++r) {
        double sum = 0.0;
        for (int c = 0; c < cols; ++c) {
            sum += grid[r * cols + c]->tilt;
        }
        rowTilt[r] = sum / static_cast<double>(cols);
    }

    int c0 = 0;
    int c1 = 0;
    double u = 0.0;
    if (cols >= 2) {
        const bool panAscending = (colPan.last() >= colPan.first());
        c0 = findMonotonicBracket(colPan, clampedPan, panAscending);
        c1 = c0 + 1;
        u = safeFraction(clampedPan, colPan[c0], colPan[c1], -6.0, 7.0);
    }

    int r0 = 0;
    int r1 = 0;
    double v = 0.0;
    if (rows >= 2) {
        const bool tiltAscending = (rowTilt.last() >= rowTilt.first());
        r0 = findMonotonicBracket(rowTilt, clampedTilt, tiltAscending);
        r1 = r0 + 1;
        v = safeFraction(clampedTilt, rowTilt[r0], rowTilt[r1], -6.0, 7.0);
    }

    const PanoramaTileMeta *t00 = grid[r0 * cols + c0];
    const PanoramaTileMeta *t10 = grid[r0 * cols + c1];
    const PanoramaTileMeta *t01 = grid[r1 * cols + c0];
    const PanoramaTileMeta *t11 = grid[r1 * cols + c1];

    for (int iter = 0; iter < 2; ++iter) {
        const double uClamp = std::clamp(u, 0.0, 1.0);
        const double vClamp = std::clamp(v, 0.0, 1.0);
        if (cols >= 2) {
            const double p0 = lerp1(t00->pan, t01->pan, vClamp);
            const double p1 = lerp1(t10->pan, t11->pan, vClamp);
            u = safeFraction(clampedPan, p0, p1, -6.0, 7.0);
        }
        if (rows >= 2) {
            const double tt0 = lerp1(t00->tilt, t10->tilt, uClamp);
            const double tt1 = lerp1(t01->tilt, t11->tilt, uClamp);
            v = safeFraction(clampedTilt, tt0, tt1, -6.0, 7.0);
        }
    }

    if (cols == 1) {
        const double leftPan = meta.invertPan ? maxPan : minPan;
        const double rightPan = meta.invertPan ? minPan : maxPan;
        const double fx = (std::abs(rightPan - leftPan) < 1e-6)
                              ? 0.5
                              : std::clamp((clampedPan - leftPan) / (rightPan - leftPan), 0.0, 1.0);
        pxOut = (std::abs(rightPan - leftPan) < 1e-6)
                    ? lerp1(t00->centerX, t01->centerX, std::clamp(v, 0.0, 1.0))
                    : fx * static_cast<double>(meta.width);
    } else {
        pxOut = std::clamp(bilerp(t00->centerX, t10->centerX, t01->centerX, t11->centerX, u, v),
                           0.0, static_cast<double>(meta.width));
    }

    if (rows == 1) {
        const double topTilt = meta.invertTilt ? minTilt : maxTilt;
        const double bottomTilt = meta.invertTilt ? maxTilt : minTilt;
        const double fy = (std::abs(bottomTilt - topTilt) < 1e-6)
                              ? 0.5
                              : std::clamp((clampedTilt - topTilt) / (bottomTilt - topTilt), 0.0, 1.0);
        pyOut = (std::abs(bottomTilt - topTilt) < 1e-6)
                    ? lerp1(t00->centerY, t10->centerY, std::clamp(u, 0.0, 1.0))
                    : fy * static_cast<double>(meta.height);
    } else {
        pyOut = std::clamp(bilerp(t00->centerY, t10->centerY, t01->centerY, t11->centerY, u, v),
                           0.0, static_cast<double>(meta.height));
    }

    return true;
}

double PanoramaMapper::zoomToMagnification(double zoom, double scanZoom, double maxOpticalZoom)
{
    const double maxOpt = std::clamp(maxOpticalZoom, 2.0, 60.0);
    const double z = std::clamp(zoom, 0.0, 1.0);
    const double sz = std::clamp(scanZoom, 0.0, 1.0);
    return std::pow(maxOpt, z - sz);
}

bool PanoramaMapper::rectToPanTiltZoom(const PanoramaMeta &meta,
                                       const QRectF &imageRectPx,
                                       double &panOut,
                                       double &tiltOut,
                                       double &zoomOut,
                                       double maxOpticalZoom)
{
    if (!meta.isValid() || imageRectPx.width() <= 0.0 || imageRectPx.height() <= 0.0) {
        return false;
    }

    const QPointF center = imageRectPx.center();
    if (!pixelToPanTilt(meta, center.x(), center.y(), panOut, tiltOut)) {
        return false;
    }

    double sumW = 0.0;
    double sumH = 0.0;
    int tileCount = 0;
    for (const PanoramaTileMeta &t : meta.tiles) {
        if (t.tileW > 0.0 && t.tileH > 0.0) {
            sumW += t.tileW;
            sumH += t.tileH;
            ++tileCount;
        }
    }
    const double baseW = (tileCount > 0) ? (sumW / static_cast<double>(tileCount)) : 160.0;
    const double baseH = (tileCount > 0) ? (sumH / static_cast<double>(tileCount)) : 90.0;

    const double magW = baseW / std::max(1.0, imageRectPx.width());
    const double magH = baseH / std::max(1.0, imageRectPx.height());
    const double relMag = std::min(magW, magH);

    const double maxOpt = std::clamp(maxOpticalZoom, 2.0, 60.0);
    const double deltaZ = std::log(std::max(0.05, relMag)) / std::log(maxOpt);
    zoomOut = std::clamp(meta.scanZoom + deltaZ, 0.0, 1.0);
    return true;
}

} // namespace netdev
