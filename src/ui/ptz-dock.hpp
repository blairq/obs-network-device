#pragma once

#include "core/device-types.hpp"
#include "onvif/onvif-client.hpp"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;

namespace netdev {

class PtzDockWidget : public QWidget {
    Q_OBJECT

public:
    explicit PtzDockWidget(QWidget *parent = nullptr);
    ~PtzDockWidget() override = default;

private slots:
    void reloadCameras();
    void onCameraSelectionChanged();
    void onActiveSourceChanged(const QString &sourceName);
    void onSpeedChanged(int value);
    void onHomeClicked();
    void onLoadPresetsClicked();
    void onGotoPresetClicked();
    void onSavePresetClicked();
    void onRemovePresetClicked();

private:
    void setupUi();
    void setControlsEnabled(bool enabled);
    bool currentCamera(PtzCameraEntry &out) const;
    void sendMove(double panDir, double tiltDir, double zoomDir);
    void sendStop();
    QPushButton *makeMoveButton(const QString &text, double panDir, double tiltDir, double zoomDir);

    OnvifClient m_client;

    QComboBox *m_cameraCombo = nullptr;
    QCheckBox *m_followSceneCheck = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QWidget *m_controlsContainer = nullptr;
    QSlider *m_speedSlider = nullptr;
    QLabel *m_speedValueLabel = nullptr;
    QListWidget *m_presetsList = nullptr;
    QLabel *m_statusLabel = nullptr;
};

} // namespace netdev
