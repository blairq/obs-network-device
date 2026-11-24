#include "panorama-view-widget.hpp"

#include "core/panorama-builder.hpp"

#include <QAction>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace netdev {

namespace {

constexpr double kPinHitRadiusPx = 12.0;
constexpr double kDragStartThresholdPx = 8.0;

} // namespace

PanoramaViewWidget::PanoramaViewWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(170);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
}

QSize PanoramaViewWidget::sizeHint() const
{
    return QSize(420, 220);
}

void PanoramaViewWidget::setPanorama(const QImage &image, const PanoramaMeta &meta)
{
    m_image = image;
    m_meta = meta;
    update();
}

void PanoramaViewWidget::clearPanorama()
{
    m_image = QImage();
    m_meta = PanoramaMeta{};
    m_hasTarget = false;
    m_hasTargetBox = false;
    m_dragging = false;
    m_dragMoved = false;
    m_hoveredPinIndex = -1;
    update();
}

void PanoramaViewWidget::setSavedPresets(const QVector<SavedPtzPreset> &presets)
{
    m_presets = presets;
    update();
}

void PanoramaViewWidget::setCurrentPosition(const PtzPosition &pos, bool valid)
{
    m_currentPos = pos;
    m_hasCurrentPos = valid && pos.hasPanTilt;
    update();
}

void PanoramaViewWidget::setTargetPosition(double pan, double tilt, bool active)
{
    m_targetPan = pan;
    m_targetTilt = tilt;
    m_hasTarget = active;
    m_hasTargetBox = false;
    update();
}

void PanoramaViewWidget::setTargetBox(const QRectF &imageRect, bool active)
{
    m_targetBoxImage = imageRect;
    m_hasTargetBox = active && imageRect.isValid() && imageRect.width() > 0.0 && imageRect.height() > 0.0;
    update();
}

void PanoramaViewWidget::setMaxOpticalZoom(double maxOpticalZoom)
{
    m_maxOpticalZoom = std::clamp(maxOpticalZoom, 2.0, 40.0);
    update();
}

QRectF PanoramaViewWidget::computeImageRect() const
{
    if (m_image.isNull() || m_image.width() <= 0 || m_image.height() <= 0) {
        return QRectF();
    }
    const QRectF avail(4.0, 4.0,
                       std::max(1.0, static_cast<double>(width()) - 8.0),
                       std::max(1.0, static_cast<double>(height()) - 8.0));
    const double imgAspect = static_cast<double>(m_image.width()) / static_cast<double>(m_image.height());
    const double availAspect = avail.width() / avail.height();

    double drawW = avail.width();
    double drawH = avail.height();
    if (availAspect > imgAspect) {
        drawW = drawH * imgAspect;
    } else {
        drawH = drawW / imgAspect;
    }

    const double x = avail.x() + 0.5 * (avail.width() - drawW);
    const double y = avail.y() + 0.5 * (avail.height() - drawH);
    return QRectF(x, y, drawW, drawH);
}

QPointF PanoramaViewWidget::imageToWidget(const QRectF &imgRect, double px, double py) const
{
    const double metaW = static_cast<double>(std::max(1, m_meta.isValid() ? m_meta.width : m_image.width()));
    const double metaH = static_cast<double>(std::max(1, m_meta.isValid() ? m_meta.height : m_image.height()));
    const double wx = imgRect.x() + (px / metaW) * imgRect.width();
    const double wy = imgRect.y() + (py / metaH) * imgRect.height();
    return QPointF(wx, wy);
}

QRectF PanoramaViewWidget::imageRectToWidget(const QRectF &imgRect, const QRectF &imageBox) const
{
    const QPointF tl = imageToWidget(imgRect, imageBox.left(), imageBox.top());
    const QPointF br = imageToWidget(imgRect, imageBox.right(), imageBox.bottom());
    return QRectF(tl, br).normalized();
}

QPointF PanoramaViewWidget::clampPointToRect(const QRectF &rect, const QPointF &pos) const
{
    return QPointF(
        std::clamp(pos.x(), rect.left(), rect.right()),
        std::clamp(pos.y(), rect.top(), rect.bottom()));
}

QRectF PanoramaViewWidget::widgetBoxToImage(const QRectF &imgRect, const QRectF &widgetBox) const
{
    if (!imgRect.isValid() || m_image.isNull()) {
        return QRectF();
    }
    const QRectF norm = widgetBox.normalized();
    const QPointF tl = clampPointToRect(imgRect, norm.topLeft());
    const QPointF br = clampPointToRect(imgRect, norm.bottomRight());
    const double metaW = static_cast<double>(std::max(1, m_meta.isValid() ? m_meta.width : m_image.width()));
    const double metaH = static_cast<double>(std::max(1, m_meta.isValid() ? m_meta.height : m_image.height()));
    const double x0 = ((tl.x() - imgRect.x()) / imgRect.width()) * metaW;
    const double y0 = ((tl.y() - imgRect.y()) / imgRect.height()) * metaH;
    const double x1 = ((br.x() - imgRect.x()) / imgRect.width()) * metaW;
    const double y1 = ((br.y() - imgRect.y()) / imgRect.height()) * metaH;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1)).normalized();
}

bool PanoramaViewWidget::widgetToImage(const QRectF &imgRect,
                                       const QPointF &pos,
                                       double &pxOut,
                                       double &pyOut) const
{
    if (!imgRect.isValid() || !imgRect.contains(pos) || m_image.isNull()) {
        return false;
    }
    const double metaW = static_cast<double>(std::max(1, m_meta.isValid() ? m_meta.width : m_image.width()));
    const double metaH = static_cast<double>(std::max(1, m_meta.isValid() ? m_meta.height : m_image.height()));
    pxOut = ((pos.x() - imgRect.x()) / imgRect.width()) * metaW;
    pyOut = ((pos.y() - imgRect.y()) / imgRect.height()) * metaH;
    return true;
}

int PanoramaViewWidget::hitTestPresetPin(const QRectF &imgRect, const QPointF &widgetPos) const
{
    if (!imgRect.isValid() || !m_meta.isValid()) {
        return -1;
    }

    int bestIdx = -1;
    double bestDist = kPinHitRadiusPx;
    for (int i = 0; i < static_cast<int>(m_presets.size()); ++i) {
        const SavedPtzPreset &preset = m_presets.at(i);
        if (!preset.hasPosition) {
            continue;
        }
        double px = 0.0;
        double py = 0.0;
        if (!PanoramaMapper::panTiltToPixel(m_meta, preset.pan, preset.tilt, px, py)) {
            continue;
        }
        const QPointF pt = imageToWidget(imgRect, px, py);
        const double dist = std::hypot(pt.x() - widgetPos.x(), pt.y() - widgetPos.y());
        if (dist <= bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void PanoramaViewWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    p.fillRect(rect(), QColor(20, 24, 31));
    p.setPen(QPen(QColor(55, 65, 81), 1.0));
    p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);

    if (m_image.isNull()) {
        p.setPen(QColor(156, 163, 175));
        p.drawText(
            rect().adjusted(14, 14, -14, -14), Qt::AlignCenter | Qt::TextWordWrap,
            QStringLiteral("Sin panoramica guardada para esta camara.\n"
                           "Toca 'Capturar panoramica...' para escanear la escena y mover la camara haciendo clic."));
        return;
    }

    const QRectF imgRect = computeImageRect();
    p.drawImage(imgRect, m_image);
    p.setPen(QPen(QColor(100, 116, 139, 180), 1.0));
    p.drawRect(imgRect);

    if (!m_meta.isValid()) {
        return;
    }

    // Target click / area marker
    if (m_hasTargetBox && m_targetBoxImage.isValid()) {
        const QRectF wBox = imageRectToWidget(imgRect, m_targetBoxImage);
        p.setBrush(QColor(34, 197, 94, 30));
        p.setPen(QPen(QColor(34, 197, 94), 2.0, Qt::DashLine));
        p.drawRect(wBox);
        const QPointF ctr = wBox.center();
        p.drawLine(QPointF(ctr.x() - 6.0, ctr.y()), QPointF(ctr.x() + 6.0, ctr.y()));
        p.drawLine(QPointF(ctr.x(), ctr.y() - 6.0), QPointF(ctr.x(), ctr.y() + 6.0));
    } else if (m_hasTarget) {
        double tx = 0.0;
        double ty = 0.0;
        if (PanoramaMapper::panTiltToPixel(m_meta, m_targetPan, m_targetTilt, tx, ty)) {
            const QPointF wPt = imageToWidget(imgRect, tx, ty);
            p.setBrush(QColor(34, 197, 94, 45));
            p.setPen(QPen(QColor(34, 197, 94), 2.0, Qt::DashLine));
            p.drawEllipse(wPt, 13.0, 13.0);
        }
    }

    // Current camera FOV frame + crosshair
    if (m_hasCurrentPos) {
        double cx = 0.0;
        double cy = 0.0;
        if (PanoramaMapper::panTiltToPixel(m_meta, m_currentPos.pan, m_currentPos.tilt, cx, cy)) {
            const QPointF cPt = imageToWidget(imgRect, cx, cy);
            const double scaleX = imgRect.width() / static_cast<double>(std::max(1, m_meta.width));
            const double scaleY = imgRect.height() / static_cast<double>(std::max(1, m_meta.height));
            const double baseW = m_meta.tiles.isEmpty() ? 160.0 : m_meta.tiles.first().tileW;
            const double baseH = m_meta.tiles.isEmpty() ? 90.0 : m_meta.tiles.first().tileH;
            const double zoomFactor = PanoramaMapper::zoomToMagnification(
                m_currentPos.zoom, m_meta.scanZoom, m_maxOpticalZoom);
            const double fovW = std::max(14.0, (baseW * scaleX) / zoomFactor);
            const double fovH = std::max(10.0, (baseH * scaleY) / zoomFactor);

            const QRectF fovRect(cPt.x() - 0.5 * fovW, cPt.y() - 0.5 * fovH, fovW, fovH);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(56, 189, 248, 220), 1.6, Qt::DashLine));
            p.drawRect(fovRect);

            p.setPen(QPen(QColor(56, 189, 248), 1.8));
            p.drawLine(QPointF(cPt.x() - 8.0, cPt.y()), QPointF(cPt.x() + 8.0, cPt.y()));
            p.drawLine(QPointF(cPt.x(), cPt.y() - 8.0), QPointF(cPt.x(), cPt.y() + 8.0));
            p.setBrush(QColor(56, 189, 248));
            p.drawEllipse(cPt, 3.5, 3.5);
        }
    }

    // Active drag selection rubberband
    if (m_dragging && m_dragMoved) {
        const QRectF wBox = QRectF(m_dragStartWidget, m_dragCurrentWidget).normalized();
        p.setBrush(QColor(16, 185, 129, 45));
        p.setPen(QPen(QColor(52, 211, 153), 2.0));
        p.drawRect(wBox);

        const QPointF ctr = wBox.center();
        p.drawLine(QPointF(ctr.x() - 7.0, ctr.y()), QPointF(ctr.x() + 7.0, ctr.y()));
        p.drawLine(QPointF(ctr.x(), ctr.y() - 7.0), QPointF(ctr.x(), ctr.y() + 7.0));

        const QRectF imgBox = widgetBoxToImage(imgRect, wBox);
        double pan = 0.0, tilt = 0.0, zoom = 0.0;
        if (PanoramaMapper::rectToPanTiltZoom(m_meta, imgBox, pan, tilt, zoom, m_maxOpticalZoom)) {
            const double scaleX = imgRect.width() / static_cast<double>(std::max(1, m_meta.width));
            const double scaleY = imgRect.height() / static_cast<double>(std::max(1, m_meta.height));
            const double baseW = m_meta.tiles.isEmpty() ? 160.0 : m_meta.tiles.first().tileW;
            const double baseH = m_meta.tiles.isEmpty() ? 90.0 : m_meta.tiles.first().tileH;
            const double zoomFactor =
                PanoramaMapper::zoomToMagnification(zoom, m_meta.scanZoom, m_maxOpticalZoom);
            const double fovW = std::max(14.0, (baseW * scaleX) / zoomFactor);
            const double fovH = std::max(10.0, (baseH * scaleY) / zoomFactor);
            const QRectF previewFov(ctr.x() - 0.5 * fovW, ctr.y() - 0.5 * fovH, fovW, fovH);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(56, 189, 248, 200), 1.4, Qt::DotLine));
            p.drawRect(previewFov);
        }
    }

    // Saved preset pins
    QFont pinFont = p.font();
    pinFont.setBold(true);
    pinFont.setPointSize(std::max(7, pinFont.pointSize() - 1));
    p.setFont(pinFont);

    int visualNumber = 0;
    for (int i = 0; i < static_cast<int>(m_presets.size()); ++i) {
        const SavedPtzPreset &preset = m_presets.at(i);
        if (!preset.hasPosition) {
            continue;
        }
        ++visualNumber;

        double px = 0.0;
        double py = 0.0;
        if (!PanoramaMapper::panTiltToPixel(m_meta, preset.pan, preset.tilt, px, py)) {
            continue;
        }
        const QPointF pt = imageToWidget(imgRect, px, py);
        const bool hovered = (i == m_hoveredPinIndex);
        const double r = hovered ? 10.5 : 8.5;

        p.setBrush(hovered ? QColor(251, 191, 36) : QColor(245, 158, 11));
        p.setPen(QPen(QColor(255, 255, 255), hovered ? 2.0 : 1.5));
        p.drawEllipse(pt, r, r);

        p.setPen(QColor(17, 24, 39));
        const QRectF textRect(pt.x() - r, pt.y() - r, r * 2.0, r * 2.0);
        p.drawText(textRect, Qt::AlignCenter, QString::number(visualNumber));
    }
}

void PanoramaViewWidget::mousePressEvent(QMouseEvent *event)
{
    const QRectF imgRect = computeImageRect();

    if (event->button() == Qt::LeftButton) {
        const int pinIdx = hitTestPresetPin(imgRect, event->position());
        if (pinIdx >= 0) {
            emit presetPinClicked(m_presets.at(pinIdx).id);
            event->accept();
            return;
        }

        if (m_meta.isValid() && imgRect.isValid() && imgRect.contains(event->position())) {
            m_dragging = true;
            m_dragMoved = false;
            m_dragStartWidget = clampPointToRect(imgRect, event->position());
            m_dragCurrentWidget = m_dragStartWidget;
            event->accept();
            return;
        }
    } else if (event->button() == Qt::RightButton) {
        if (m_dragging) {
            m_dragging = false;
            m_dragMoved = false;
            QToolTip::hideText();
            update();
            event->accept();
            return;
        }

        QMenu menu(this);
        double px = 0.0;
        double py = 0.0;
        double pan = 0.0;
        double tilt = 0.0;
        const bool hasClickPoint = m_meta.isValid() &&
                                   widgetToImage(imgRect, event->position(), px, py) &&
                                   PanoramaMapper::pixelToPanTilt(m_meta, px, py, pan, tilt);

        QAction *moveAct = nullptr;
        if (hasClickPoint) {
            moveAct = menu.addAction(
                QStringLiteral("Mover camara a (P=%1, T=%2)")
                    .arg(pan, 0, 'f', 2)
                    .arg(tilt, 0, 'f', 2));
        }
        QAction *saveAct = menu.addAction(QStringLiteral("Guardar vista actual como preset..."));

        QAction *chosen = menu.exec(event->globalPosition().toPoint());
        if (chosen && chosen == moveAct) {
            setTargetPosition(pan, tilt, true);
            emit pointClicked(pan, tilt);
        } else if (chosen && chosen == saveAct) {
            emit savePresetRequested();
        }
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void PanoramaViewWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QRectF imgRect = computeImageRect();

    if (m_dragging) {
        if (!imgRect.isValid() || !m_meta.isValid()) {
            return;
        }
        m_dragCurrentWidget = clampPointToRect(imgRect, event->position());
        if (!m_dragMoved &&
            std::hypot(m_dragCurrentWidget.x() - m_dragStartWidget.x(),
                       m_dragCurrentWidget.y() - m_dragStartWidget.y()) >= kDragStartThresholdPx) {
            m_dragMoved = true;
        }
        update();

        if (m_dragMoved) {
            const QRectF imgBox =
                widgetBoxToImage(imgRect, QRectF(m_dragStartWidget, m_dragCurrentWidget));
            double pan = 0.0, tilt = 0.0, zoom = 0.0;
            if (PanoramaMapper::rectToPanTiltZoom(
                    m_meta, imgBox, pan, tilt, zoom, m_maxOpticalZoom)) {
                const double mag = PanoramaMapper::zoomToMagnification(
                    zoom, m_meta.scanZoom, m_maxOpticalZoom);
                QToolTip::showText(
                    event->globalPosition().toPoint(),
                    QStringLiteral("Encuadrar cuadro -> Pan: %1  Tilt: %2  Zoom: %3 (%4x)")
                        .arg(pan, 0, 'f', 2)
                        .arg(tilt, 0, 'f', 2)
                        .arg(zoom, 0, 'f', 2)
                        .arg(mag, 0, 'f', 1),
                    this);
            }
        }
        event->accept();
        return;
    }

    const int pinIdx = hitTestPresetPin(imgRect, event->position());
    if (pinIdx != m_hoveredPinIndex) {
        m_hoveredPinIndex = pinIdx;
        update();
    }

    if (pinIdx >= 0) {
        setCursor(Qt::PointingHandCursor);
        const SavedPtzPreset &preset = m_presets.at(pinIdx);
        QToolTip::showText(
            event->globalPosition().toPoint(),
            QStringLiteral("%1\nPan: %2  Tilt: %3  Zoom: %4")
                .arg(preset.name)
                .arg(preset.pan, 0, 'f', 2)
                .arg(preset.tilt, 0, 'f', 2)
                .arg(preset.zoom, 0, 'f', 2),
            this);
        return;
    }

    double px = 0.0;
    double py = 0.0;
    double pan = 0.0;
    double tilt = 0.0;
    if (m_meta.isValid() && widgetToImage(imgRect, event->position(), px, py) &&
        PanoramaMapper::pixelToPanTilt(m_meta, px, py, pan, tilt)) {
        setCursor(Qt::CrossCursor);
        QToolTip::showText(
            event->globalPosition().toPoint(),
            QStringLiteral("Clic: mover | Arrastrar: encuadrar cuadro (PTZ)\nPan: %1  Tilt: %2")
                .arg(pan, 0, 'f', 2)
                .arg(tilt, 0, 'f', 2),
            this);
    } else {
        setCursor(Qt::ArrowCursor);
        QToolTip::hideText();
    }
}

void PanoramaViewWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        const bool wasBox = m_dragMoved;
        const QRectF imgRect = computeImageRect();
        const QPointF endPt = imgRect.isValid() ? clampPointToRect(imgRect, event->position())
                                                : event->position();
        m_dragging = false;
        m_dragMoved = false;

        if (!m_meta.isValid() || !imgRect.isValid()) {
            update();
            event->accept();
            return;
        }

        if (!wasBox) {
            const double metaW = static_cast<double>(std::max(1, m_meta.width));
            const double metaH = static_cast<double>(std::max(1, m_meta.height));
            const double px = ((endPt.x() - imgRect.x()) / imgRect.width()) * metaW;
            const double py = ((endPt.y() - imgRect.y()) / imgRect.height()) * metaH;
            double pan = 0.0, tilt = 0.0;
            if (PanoramaMapper::pixelToPanTilt(m_meta, px, py, pan, tilt)) {
                setTargetPosition(pan, tilt, true);
                emit pointClicked(pan, tilt);
            } else {
                update();
            }
        } else {
            const QRectF imgBox =
                widgetBoxToImage(imgRect, QRectF(m_dragStartWidget, endPt));
            double pan = 0.0, tilt = 0.0, zoom = 0.0;
            if (PanoramaMapper::rectToPanTiltZoom(
                    m_meta, imgBox, pan, tilt, zoom, m_maxOpticalZoom)) {
                m_targetPan = pan;
                m_targetTilt = tilt;
                m_hasTarget = true;
                m_targetBoxImage = imgBox;
                m_hasTargetBox = true;
                update();
                emit areaSelected(pan, tilt, zoom, imgBox);
            } else {
                update();
            }
        }
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void PanoramaViewWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_dragging) {
        m_dragging = false;
        m_dragMoved = false;
        QToolTip::hideText();
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PanoramaViewWidget::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    if (m_hoveredPinIndex >= 0) {
        m_hoveredPinIndex = -1;
        update();
    }
    setCursor(Qt::ArrowCursor);
}

void PanoramaViewWidget::wheelEvent(QWheelEvent *event)
{
    if (!hasPanorama()) {
        QWidget::wheelEvent(event);
        return;
    }
    const int yDelta = event->angleDelta().y();
    if (yDelta != 0) {
        emit zoomRequested(yDelta > 0 ? 1 : -1);
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

} // namespace netdev
