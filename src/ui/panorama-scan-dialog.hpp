#pragma once

#include "core/device-types.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace netdev {

class OnvifClient;

class PanoramaScanDialog : public QDialog {
    Q_OBJECT

public:
    explicit PanoramaScanDialog(OnvifClient *client,
                                const PtzCameraEntry &cam,
                                bool defaultInvertPan,
                                bool defaultInvertTilt,
                                FrameCaptureMode defaultCaptureMode,
                                QWidget *parent = nullptr);

    PanoramaScanSettings settings() const;

private slots:
    void onGridPresetChanged(int index);
    void onGridSpinChanged();
    void onApplyDetectedLimits();

private:
    void setupUi(const PtzCameraEntry &cam,
                 bool defaultInvertPan,
                 bool defaultInvertTilt,
                 FrameCaptureMode defaultCaptureMode);
    void fetchCameraLimits(OnvifClient *client, const PtzCameraEntry &cam);

    QLabel *m_limitsLabel = nullptr;
    QPushButton *m_applyLimitsBtn = nullptr;

    QComboBox *m_presetCombo = nullptr;
    QSpinBox *m_colsSpin = nullptr;
    QSpinBox *m_rowsSpin = nullptr;

    QDoubleSpinBox *m_panMinSpin = nullptr;
    QDoubleSpinBox *m_panMaxSpin = nullptr;
    QDoubleSpinBox *m_tiltMinSpin = nullptr;
    QDoubleSpinBox *m_tiltMaxSpin = nullptr;
    QDoubleSpinBox *m_scanZoomSpin = nullptr;
    QSpinBox *m_settleDelaySpin = nullptr;

    QComboBox *m_captureModeCombo = nullptr;
    QCheckBox *m_invertPanCheck = nullptr;
    QCheckBox *m_invertTiltCheck = nullptr;

    PtzLimits m_detectedLimits;
};

} // namespace netdev
