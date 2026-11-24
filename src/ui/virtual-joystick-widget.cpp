#include "virtual-joystick-widget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace netdev {

VirtualJoystickWidget::VirtualJoystickWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(150, 150);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Arrastra en cualquier direccion para mover Pan/Tilt de forma proporcional."));
}

void VirtualJoystickWidget::setDeadzone(double dz)
{
    m_deadzone = std::clamp(dz, 0.0, 0.35);
    update();
}

QSize VirtualJoystickWidget::sizeHint() const
{
    return QSize(176, 176);
}

QPointF VirtualJoystickWidget::padCenter() const
{
    return QPointF(0.5 * static_cast<double>(width()), 0.5 * static_cast<double>(height()));
}

double VirtualJoystickWidget::padRadius() const
{
    const double minSide = static_cast<double>(std::min(width(), height()));
    return std::max(24.0, minSide * 0.5 - 16.0);
}

void VirtualJoystickWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QPointF center = padCenter();
    const double radius = padRadius();

    // Base well
    QRadialGradient bgGrad(center, radius);
    bgGrad.setColorAt(0.0, QColor(36, 41, 51));
    bgGrad.setColorAt(0.85, QColor(24, 28, 36));
    bgGrad.setColorAt(1.0, QColor(18, 21, 27));
    p.setBrush(bgGrad);
    p.setPen(QPen(QColor(72, 82, 99), 2.0));
    p.drawEllipse(center, radius, radius);

    // Reference rings (50% and deadzone)
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(68, 77, 92, 160), 1.0, Qt::DashLine));
    p.drawEllipse(center, radius * 0.5, radius * 0.5);
    if (m_deadzone > 0.01) {
        p.setPen(QPen(QColor(95, 107, 126, 130), 1.0, Qt::DotLine));
        p.drawEllipse(center, radius * m_deadzone, radius * m_deadzone);
    }

    // Crosshair lines
    p.setPen(QPen(QColor(78, 88, 104, 170), 1.0));
    p.drawLine(QPointF(center.x() - radius + 4.0, center.y()),
               QPointF(center.x() + radius - 4.0, center.y()));
    p.drawLine(QPointF(center.x(), center.y() - radius + 4.0),
               QPointF(center.x(), center.y() + radius - 4.0));

    // Knob position
    const QPointF knobCenter(center.x() + m_rawX * (radius - 14.0),
                             center.y() - m_rawY * (radius - 14.0));

    // Active deflection vector
    if (m_dragging && (std::abs(m_rawX) > 1e-3 || std::abs(m_rawY) > 1e-3)) {
        p.setPen(QPen(QColor(56, 189, 248, 210), 2.5));
        p.drawLine(center, knobCenter);
    }

    // Knob circle
    const double knobR = std::clamp(radius * 0.24, 12.0, 22.0);
    QRadialGradient knobGrad(knobCenter - QPointF(knobR * 0.25, knobR * 0.25), knobR * 1.2);
    if (m_dragging) {
        knobGrad.setColorAt(0.0, QColor(125, 211, 252));
        knobGrad.setColorAt(0.6, QColor(14, 165, 233));
        knobGrad.setColorAt(1.0, QColor(3, 105, 161));
    } else {
        knobGrad.setColorAt(0.0, QColor(148, 163, 184));
        knobGrad.setColorAt(0.6, QColor(71, 85, 105));
        knobGrad.setColorAt(1.0, QColor(30, 41, 59));
    }
    p.setBrush(knobGrad);
    p.setPen(QPen(QColor(226, 232, 240, 180), 1.5));
    p.drawEllipse(knobCenter, knobR, knobR);
}

void VirtualJoystickWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    setCursor(Qt::ClosedHandCursor);
    updateKnobFromPos(event->position());
    event->accept();
}

void VirtualJoystickWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    updateKnobFromPos(event->position());
    event->accept();
}

void VirtualJoystickWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    m_rawX = 0.0;
    m_rawY = 0.0;
    setCursor(Qt::OpenHandCursor);
    update();
    emit released();
    event->accept();
}

void VirtualJoystickWidget::updateKnobFromPos(const QPointF &pos)
{
    const QPointF center = padCenter();
    const double maxTravel = std::max(10.0, padRadius() - 14.0);
    const double dx = (pos.x() - center.x()) / maxTravel;
    const double dy = -(pos.y() - center.y()) / maxTravel; // +Y up

    const double mag = std::hypot(dx, dy);
    if (mag > 1.0) {
        m_rawX = dx / mag;
        m_rawY = dy / mag;
    } else {
        m_rawX = dx;
        m_rawY = dy;
    }
    update();

    const double outMag = std::hypot(m_rawX, m_rawY);
    if (outMag <= m_deadzone) {
        emit moved(0.0, 0.0);
        return;
    }

    const double scaled = std::min(1.0, (outMag - m_deadzone) / std::max(0.05, 1.0 - m_deadzone));
    const double smooth = std::pow(scaled, 1.25);
    emit moved((m_rawX / outMag) * smooth, (m_rawY / outMag) * smooth);
}

} // namespace netdev
