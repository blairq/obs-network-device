#pragma once

#include <QPointF>
#include <QWidget>

namespace netdev {

class VirtualJoystickWidget : public QWidget {
    Q_OBJECT

public:
    explicit VirtualJoystickWidget(QWidget *parent = nullptr);

    double deadzone() const { return m_deadzone; }
    void setDeadzone(double dz);

signals:
    void moved(double x, double y);
    void released();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    QSize sizeHint() const override;

private:
    void updateKnobFromPos(const QPointF &pos);
    QPointF padCenter() const;
    double padRadius() const;

    bool m_dragging = false;
    double m_deadzone = 0.08;
    double m_rawX = 0.0;
    double m_rawY = 0.0; // +1 up, -1 down
};

} // namespace netdev
