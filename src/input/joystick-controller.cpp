#include "joystick-controller.hpp"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <Xinput.h>
#endif

namespace netdev {

namespace {

constexpr int kPollIntervalMs = 20; // 50 Hz
constexpr double kEpsilonDelta = 0.035;

void applyRadialDeadzone(double rawX, double rawY, double deadzone, double &outX, double &outY)
{
    const double mag = std::hypot(rawX, rawY);
    if (mag <= deadzone || mag < 1e-9) {
        outX = 0.0;
        outY = 0.0;
        return;
    }
    const double norm = std::min(1.0, (mag - deadzone) / std::max(0.05, 1.0 - deadzone));
    const double curved = std::pow(norm, 1.35);
    outX = (rawX / mag) * curved;
    outY = (rawY / mag) * curved;
}

double applyAxisDeadzone(double raw, double deadzone)
{
    const double absV = std::abs(raw);
    if (absV <= deadzone) {
        return 0.0;
    }
    const double norm = std::min(1.0, (absV - deadzone) / std::max(0.05, 1.0 - deadzone));
    const double curved = std::pow(norm, 1.35);
    return (raw >= 0.0) ? curved : -curved;
}

#ifdef _WIN32
using FnXInputGetState = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

double normalizeSignedShort(SHORT v)
{
    return (v < 0) ? (static_cast<double>(v) / 32768.0) : (static_cast<double>(v) / 32767.0);
}

double normalizeWinmmAxis(DWORD pos, UINT minVal, UINT maxVal)
{
    if (maxVal <= minVal) {
        return 0.0;
    }
    const double norm = (static_cast<double>(pos) - static_cast<double>(minVal)) /
                        (static_cast<double>(maxVal) - static_cast<double>(minVal));
    return std::clamp(norm * 2.0 - 1.0, -1.0, 1.0);
}
#endif

} // namespace

JoystickController::JoystickController(QObject *parent) : QObject(parent)
{
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &JoystickController::onPollTimer);

#ifdef _WIN32
    HMODULE mod = LoadLibraryW(L"xinput1_4.dll");
    if (!mod) {
        mod = LoadLibraryW(L"xinput1_3.dll");
    }
    if (!mod) {
        mod = LoadLibraryW(L"xinput9_1_0.dll");
    }
    if (mod) {
        m_xinputModule = mod;
        m_xinputGetState = reinterpret_cast<void *>(GetProcAddress(mod, "XInputGetState"));
    }
#endif
}

JoystickController::~JoystickController()
{
    m_pollTimer.stop();
#ifdef _WIN32
    if (m_xinputModule) {
        FreeLibrary(static_cast<HMODULE>(m_xinputModule));
        m_xinputModule = nullptr;
        m_xinputGetState = nullptr;
    }
#endif
}

QVector<JoystickDeviceInfo> JoystickController::enumerateDevices() const
{
    QVector<JoystickDeviceInfo> list;

#ifdef _WIN32
    if (m_xinputGetState) {
        auto fn = reinterpret_cast<FnXInputGetState>(m_xinputGetState);
        for (DWORD idx = 0; idx < 4; ++idx) {
            XINPUT_STATE st{};
            if (fn(idx, &st) == ERROR_SUCCESS) {
                JoystickDeviceInfo info;
                info.id = QStringLiteral("xinput:%1").arg(idx);
                info.name = QStringLiteral("Gamepad XInput #%1").arg(idx + 1);
                info.isXInput = true;
                info.index = static_cast<int>(idx);
                list.push_back(info);
            }
        }
    }

    const UINT numDevs = joyGetNumDevs();
    const UINT limit = std::min<UINT>(numDevs, 16);
    for (UINT idx = 0; idx < limit; ++idx) {
        JOYINFOEX ji{};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(idx, &ji) == JOYERR_NOERROR) {
            JOYCAPSW caps{};
            QString devName = QStringLiteral("Joystick USB #%1").arg(idx + 1);
            if (joyGetDevCapsW(idx, &caps, sizeof(caps)) == JOYERR_NOERROR) {
                const QString prod = QString::fromWCharArray(caps.szPname).trimmed();
                if (!prod.isEmpty()) {
                    devName = QStringLiteral("%1 (#%2)").arg(prod).arg(idx + 1);
                }
            }
            JoystickDeviceInfo info;
            info.id = QStringLiteral("winmm:%1").arg(idx);
            info.name = devName;
            info.isXInput = false;
            info.index = static_cast<int>(idx);
            list.push_back(info);
        }
    }
#endif

    return list;
}

void JoystickController::setSelectedDeviceId(const QString &deviceId)
{
    const QString trimmed = deviceId.trimmed();
    if (m_selectedDeviceId == trimmed) {
        return;
    }

    const bool wasMoving = m_wasMoving;
    m_selectedDeviceId = trimmed;
    m_lastPan = 0.0;
    m_lastTilt = 0.0;
    m_lastZoom = 0.0;
    m_wasMoving = false;
    m_prevButtons = 0;
    m_winmmZSeenNeutral = false;
    m_winmmLastDevId.clear();

    if (wasMoving) {
        emit axisChanged(0.0, 0.0, 0.0);
    }

    if (m_selectedDeviceId.isEmpty()) {
        m_pollTimer.stop();
        m_lastStatus = QStringLiteral("Joystick fisico desactivado");
        emit connectionStatusChanged(m_lastStatus);
    } else {
        if (!m_pollTimer.isActive()) {
            m_pollTimer.start();
        }
        onPollTimer();
    }
}

void JoystickController::setDeadzone(double deadzone)
{
    m_deadzone = std::clamp(deadzone, 0.02, 0.45);
}

void JoystickController::onPollTimer()
{
    if (m_selectedDeviceId.isEmpty()) {
        return;
    }

    double pan = 0.0;
    double tilt = 0.0;
    double zoom = 0.0;
    quint32 buttons = 0;
    QString statusText;

    const bool connected = pollSelectedDevice(pan, tilt, zoom, buttons, statusText);
    if (statusText != m_lastStatus) {
        m_lastStatus = statusText;
        emit connectionStatusChanged(statusText);
    }

    if (!connected) {
        if (m_wasMoving) {
            m_wasMoving = false;
            m_lastPan = 0.0;
            m_lastTilt = 0.0;
            m_lastZoom = 0.0;
            emit axisChanged(0.0, 0.0, 0.0);
        }
        m_prevButtons = 0;
        return;
    }

    const quint32 pressedNow = buttons & (~m_prevButtons);
    m_prevButtons = buttons;
    for (int i = 0; i < 4; ++i) {
        if ((pressedNow & (1u << i)) != 0) {
            emit presetTriggered(i);
        }
    }
    if ((pressedNow & (1u << 4)) != 0) {
        emit homeTriggered();
    }

    const bool moving = (std::abs(pan) > 1e-4) || (std::abs(tilt) > 1e-4) ||
                        (std::abs(zoom) > 1e-4);

    if (!moving) {
        if (m_wasMoving) {
            m_wasMoving = false;
            m_lastPan = 0.0;
            m_lastTilt = 0.0;
            m_lastZoom = 0.0;
            emit axisChanged(0.0, 0.0, 0.0);
        }
        return;
    }

    if (!m_wasMoving ||
        std::abs(pan - m_lastPan) >= kEpsilonDelta ||
        std::abs(tilt - m_lastTilt) >= kEpsilonDelta ||
        std::abs(zoom - m_lastZoom) >= kEpsilonDelta) {
        m_wasMoving = true;
        m_lastPan = pan;
        m_lastTilt = tilt;
        m_lastZoom = zoom;
        emit axisChanged(pan, tilt, zoom);
    }
}

bool JoystickController::pollSelectedDevice(double &panOut,
                                              double &tiltOut,
                                              double &zoomOut,
                                              quint32 &buttonsMaskOut,
                                              QString &statusTextOut)
{
    panOut = 0.0;
    tiltOut = 0.0;
    zoomOut = 0.0;
    buttonsMaskOut = 0;

#ifndef _WIN32
    statusTextOut = QStringLiteral("Soporte de joystick fisico disponible en Windows");
    return false;
#else
    auto pollXInput = [&](DWORD idx) -> bool {
        if (!m_xinputGetState) {
            return false;
        }
        auto fn = reinterpret_cast<FnXInputGetState>(m_xinputGetState);
        XINPUT_STATE st{};
        if (fn(idx, &st) != ERROR_SUCCESS) {
            return false;
        }

        const double lx = normalizeSignedShort(st.Gamepad.sThumbLX);
        const double ly = normalizeSignedShort(st.Gamepad.sThumbLY);
        applyRadialDeadzone(lx, ly, m_deadzone, panOut, tiltOut);

        if (std::abs(panOut) < 1e-4 && std::abs(tiltOut) < 1e-4) {
            if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0) panOut -= 0.55;
            if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0) panOut += 0.55;
            if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0) tiltOut += 0.55;
            if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0) tiltOut -= 0.55;
        }

        const double ry = applyAxisDeadzone(normalizeSignedShort(st.Gamepad.sThumbRY), m_deadzone);
        const double trigZoom = (static_cast<double>(st.Gamepad.bRightTrigger) -
                                 static_cast<double>(st.Gamepad.bLeftTrigger)) /
                                255.0;
        const double trigClean = applyAxisDeadzone(trigZoom, 0.08);
        zoomOut = (std::abs(trigClean) > std::abs(ry)) ? trigClean : ry;

        if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0) buttonsMaskOut |= (1u << 0);
        if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0) buttonsMaskOut |= (1u << 1);
        if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0) buttonsMaskOut |= (1u << 2);
        if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_Y) != 0) buttonsMaskOut |= (1u << 3);
        if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0) buttonsMaskOut |= (1u << 4);

        statusTextOut = QStringLiteral("Gamepad XInput #%1 conectado").arg(idx + 1);
        return true;
    };

    auto pollWinmm = [&](UINT idx) -> bool {
        JOYINFOEX ji{};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(idx, &ji) != JOYERR_NOERROR) {
            return false;
        }

        JOYCAPSW caps{};
        if (joyGetDevCapsW(idx, &caps, sizeof(caps)) != JOYERR_NOERROR) {
            caps.wXmin = 0; caps.wXmax = 65535;
            caps.wYmin = 0; caps.wYmax = 65535;
            caps.wZmin = 0; caps.wZmax = 65535;
            caps.wRmin = 0; caps.wRmax = 65535;
        }

        const double rawX = normalizeWinmmAxis(ji.dwXpos, caps.wXmin, caps.wXmax);
        const double rawY = -normalizeWinmmAxis(ji.dwYpos, caps.wYmin, caps.wYmax);
        applyRadialDeadzone(rawX, rawY, m_deadzone, panOut, tiltOut);

        if (std::abs(panOut) < 1e-4 && std::abs(tiltOut) < 1e-4 &&
            ji.dwPOV != JOY_POVCENTERED && ji.dwPOV < 36000) {
            const double rad = (static_cast<double>(ji.dwPOV) / 100.0) * (3.14159265358979 / 180.0);
            panOut = 0.55 * std::sin(rad);
            tiltOut = 0.55 * std::cos(rad);
        }

        const QString curKey = QStringLiteral("winmm:%1").arg(idx);
        if (m_winmmLastDevId != curKey) {
            m_winmmLastDevId = curKey;
            m_winmmZSeenNeutral = false;
        }

        if ((caps.wCaps & JOYCAPS_HASR) != 0) {
            const double rawR = normalizeWinmmAxis(ji.dwRpos, caps.wRmin, caps.wRmax);
            zoomOut = applyAxisDeadzone(rawR, m_deadzone);
        } else if ((caps.wCaps & JOYCAPS_HASZ) != 0) {
            const double rawZ = normalizeWinmmAxis(ji.dwZpos, caps.wZmin, caps.wZmax);
            if (!m_winmmZSeenNeutral && std::abs(rawZ) < 0.35) {
                m_winmmZSeenNeutral = true;
            }
            if (m_winmmZSeenNeutral) {
                zoomOut = applyAxisDeadzone(-rawZ, m_deadzone);
            }
        }

        for (int b = 0; b < 5; ++b) {
            if ((ji.dwButtons & (1u << b)) != 0) {
                buttonsMaskOut |= (1u << b);
            }
        }

        statusTextOut = QStringLiteral("Joystick USB #%1 conectado").arg(idx + 1);
        return true;
    };

    if (m_selectedDeviceId == QStringLiteral("auto")) {
        for (DWORD idx = 0; idx < 4; ++idx) {
            if (pollXInput(idx)) {
                return true;
            }
        }
        const UINT numDevs = std::min<UINT>(joyGetNumDevs(), 16);
        for (UINT idx = 0; idx < numDevs; ++idx) {
            if (pollWinmm(idx)) {
                return true;
            }
        }
        statusTextOut = QStringLiteral("Ningun joystick o gamepad detectado");
        return false;
    }

    if (m_selectedDeviceId.startsWith(QStringLiteral("xinput:"))) {
        bool ok = false;
        const int idx = m_selectedDeviceId.mid(7).toInt(&ok);
        if (ok && idx >= 0 && idx < 4 && pollXInput(static_cast<DWORD>(idx))) {
            return true;
        }
        statusTextOut = QStringLiteral("Gamepad XInput desconectado");
        return false;
    }

    if (m_selectedDeviceId.startsWith(QStringLiteral("winmm:"))) {
        bool ok = false;
        const int idx = m_selectedDeviceId.mid(6).toInt(&ok);
        if (ok && idx >= 0 && idx < 16 && pollWinmm(static_cast<UINT>(idx))) {
            return true;
        }
        statusTextOut = QStringLiteral("Joystick USB desconectado");
        return false;
    }

    statusTextOut = QStringLiteral("Dispositivo de joystick invalido");
    return false;
#endif
}

} // namespace netdev
