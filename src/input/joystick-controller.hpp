#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

namespace netdev {

struct JoystickDeviceInfo {
    QString id;
    QString name;
    bool isXInput = false;
    int index = -1;
};

class JoystickController : public QObject {
    Q_OBJECT

public:
    explicit JoystickController(QObject *parent = nullptr);
    ~JoystickController() override;

    QVector<JoystickDeviceInfo> enumerateDevices() const;

    QString selectedDeviceId() const { return m_selectedDeviceId; }
    void setSelectedDeviceId(const QString &deviceId);

    double deadzone() const { return m_deadzone; }
    void setDeadzone(double deadzone);

signals:
    void axisChanged(double pan, double tilt, double zoom);
    void presetTriggered(int presetIndex);
    void homeTriggered();
    void connectionStatusChanged(const QString &status);

private slots:
    void onPollTimer();

private:
    bool pollSelectedDevice(double &panOut,
                            double &tiltOut,
                            double &zoomOut,
                            quint32 &buttonsMaskOut,
                            QString &statusTextOut);

    QTimer m_pollTimer;
    QString m_selectedDeviceId;
    double m_deadzone = 0.15;

    double m_lastPan = 0.0;
    double m_lastTilt = 0.0;
    double m_lastZoom = 0.0;
    bool m_wasMoving = false;
    quint32 m_prevButtons = 0;
    QString m_lastStatus;

    bool m_winmmZSeenNeutral = false;
    QString m_winmmLastDevId;

#ifdef _WIN32
    void *m_xinputModule = nullptr;
    void *m_xinputGetState = nullptr;
#endif
};

} // namespace netdev
