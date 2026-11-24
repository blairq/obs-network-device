#pragma once

#include "core/device-types.hpp"
#include "core/panorama-scanner.hpp"
#include "core/panorama-store.hpp"
#include "input/joystick-controller.hpp"
#include "onvif/onvif-client.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QTimer>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QDialog;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QTabWidget;

namespace netdev {

class PanoramaViewWidget;
class VirtualJoystickWidget;

class PtzDockWidget : public QWidget {
    Q_OBJECT

public:
    explicit PtzDockWidget(const QString &storageBaseDir, QWidget *parent = nullptr);
    ~PtzDockWidget() override;

    void selectCameraBySourceUuid(const QString &sourceUuid);

private slots:
    void reloadCameras();
    void onCameraSelectionChanged();
    void onActiveSourceChanged(const QString &sourceName);
    void onSpeedChanged(int value);
    void onHomeClicked();
    void onRefreshPositionClicked();

    // Panorama
    void onCapturePanoramaClicked();
    void onCancelScanClicked();
    void onScanProgress(int completed, int total, const QString &statusText, const QImage &lastTile);
    void onScanFinished(bool ok, const QImage &panorama, const PanoramaMeta &meta, const QString &error);
    void onScanCancelled();
    void onPanoramaPointClicked(double pan, double tilt);
    void onPanoramaAreaSelected(double pan, double tilt, double zoom, const QRectF &imageRect);
    void onMaxOpticalZoomChanged(double value);
    void onPanoramaPresetPinClicked(const QString &presetId);
    void onPanoramaZoomRequested(int deltaSteps);
    void onOpenBigPanoramaClicked();

    // Virtual + Physical Joystick
    void onVirtualJoystickMoved(double x, double y);
    void onVirtualJoystickReleased();
    void onJoystickDeviceChanged();
    void onRefreshJoysticksClicked();
    void onPhysicalJoystickAxis(double pan, double tilt, double zoom);
    void onPhysicalJoystickPreset(int index);
    void onInvertAxisToggled();

    // Visual Saved Presets
    void onSaveVisualPresetClicked();
    void onGotoVisualPresetClicked();
    void onUpdateVisualPresetPhotoClicked();
    void onRenameVisualPresetClicked();
    void onDeleteVisualPresetClicked();

    // Camera-internal ONVIF Presets
    void onLoadPresetsClicked();
    void onGotoPresetClicked();
    void onSavePresetClicked();
    void onRemovePresetClicked();

private:
    void setupUi();
    void setControlsEnabled(bool enabled);
    bool currentCamera(PtzCameraEntry &out) const;
    void loadCurrentCameraProfile();
    void refreshVisualPresetsList();
    void syncPanoramaViews();

    void sendMove(double panDir, double tiltDir, double zoomDir);
    void sendStop();
    void updateContinuousMotion(bool forceImmediate = false);
    void scheduleStatusRefresh();
    void queryCurrentStatus();

    void captureCurrentFrameForCamera(const PtzCameraEntry &cam,
                                      std::function<void(const QImage &)> cb);
    void recallSavedPreset(const SavedPtzPreset &preset);
    QPushButton *makeMoveButton(const QString &text, double zoomDir);
    QWidget *createPanoramaTab(QWidget *parent);
    QWidget *createJoystickTab(QWidget *parent);
    QWidget *createPresetsTab(QWidget *parent);

    OnvifClient m_client;
    PanoramaStore m_store;
    PanoramaScanner m_scanner;
    JoystickController m_joystick;

    CameraPtzProfile m_activeProfile;
    QImage m_activePanoramaImage;
    PtzPosition m_currentPos;
    bool m_hasCurrentPos = false;
    double m_targetPan = 0.0;
    double m_targetTilt = 0.0;
    bool m_hasTarget = false;
    QRectF m_targetBoxImage;
    bool m_hasTargetBox = false;

    double m_vJoyPan = 0.0;
    double m_vJoyTilt = 0.0;
    double m_pJoyPan = 0.0;
    double m_pJoyTilt = 0.0;
    double m_pJoyZoom = 0.0;
    double m_btnZoomDir = 0.0;
    bool m_isMovingContinuous = false;
    int m_movingPollCount = 0;
    QElapsedTimer m_moveThrottleTimer;
    QTimer m_wheelStopTimer;
    QTimer m_statusRefreshTimer1;
    QTimer m_statusRefreshTimer2;

    QComboBox *m_cameraCombo = nullptr;
    QCheckBox *m_followSceneCheck = nullptr;
    QPushButton *m_homeBtn = nullptr;
    QPushButton *m_refreshPosBtn = nullptr;
    QLabel *m_coordsLabel = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QWidget *m_controlsContainer = nullptr;
    QTabWidget *m_tabs = nullptr;

    // Panorama tab
    PanoramaViewWidget *m_panoramaView = nullptr;
    QWidget *m_scanPanel = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    QLabel *m_scanThumbLabel = nullptr;
    QLabel *m_scanStatusLabel = nullptr;
    QPushButton *m_cancelScanBtn = nullptr;
    QPushButton *m_capturePanoBtn = nullptr;
    QPushButton *m_bigViewBtn = nullptr;
    QDoubleSpinBox *m_maxOpticalZoomSpin = nullptr;
    QPointer<QDialog> m_bigViewDialog;
    QPointer<PanoramaViewWidget> m_bigPanoramaView;

    // Joystick tab
    VirtualJoystickWidget *m_virtualJoystick = nullptr;
    QSlider *m_speedSlider = nullptr;
    QLabel *m_speedValueLabel = nullptr;
    QCheckBox *m_invertPanCheck = nullptr;
    QCheckBox *m_invertTiltCheck = nullptr;
    QComboBox *m_joystickCombo = nullptr;
    QLabel *m_joystickStatusLabel = nullptr;

    // Presets tab
    QListWidget *m_visualPresetsList = nullptr;
    QListWidget *m_presetsList = nullptr;
    QLabel *m_statusLabel = nullptr;
};

} // namespace netdev
