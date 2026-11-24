#pragma once

#include "core/device-types.hpp"

#include <QImage>
#include <QRectF>
#include <QVector>
#include <QWidget>

namespace netdev {

class PanoramaViewWidget : public QWidget {
    Q_OBJECT

public:
    explicit PanoramaViewWidget(QWidget *parent = nullptr);

    void setPanorama(const QImage &image, const PanoramaMeta &meta);
    void clearPanorama();

    void setSavedPresets(const QVector<SavedPtzPreset> &presets);
    void setCurrentPosition(const PtzPosition &pos, bool valid);
    void setTargetPosition(double pan, double tilt, bool active);

    bool hasPanorama() const { return !m_image.isNull() && m_meta.isValid(); }
    const PanoramaMeta &panoramaMeta() const { return m_meta; }

signals:
    void pointClicked(double pan, double tilt);
    void presetPinClicked(const QString &presetId);
    void savePresetRequested();
    void zoomRequested(int deltaSteps);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    QSize sizeHint() const override;

private:
    QRectF computeImageRect() const;
    QPointF imageToWidget(const QRectF &imgRect, double px, double py) const;
    bool widgetToImage(const QRectF &imgRect, const QPointF &pos, double &pxOut, double &pyOut) const;
    int hitTestPresetPin(const QRectF &imgRect, const QPointF &widgetPos) const;

    QImage m_image;
    PanoramaMeta m_meta;
    QVector<SavedPtzPreset> m_presets;

    PtzPosition m_currentPos;
    bool m_hasCurrentPos = false;

    double m_targetPan = 0.0;
    double m_targetTilt = 0.0;
    bool m_hasTarget = false;

    int m_hoveredPinIndex = -1;
};

} // namespace netdev
