#include "panorama-scan-dialog.hpp"

#include "onvif/onvif-client.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace netdev {

PanoramaScanDialog::PanoramaScanDialog(OnvifClient *client,
                                       const PtzCameraEntry &cam,
                                       bool defaultInvertPan,
                                       bool defaultInvertTilt,
                                       FrameCaptureMode defaultCaptureMode,
                                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Capturar panoramica PTZ"));
    setMinimumWidth(420);
    setupUi(cam, defaultInvertPan, defaultInvertTilt, defaultCaptureMode);
    fetchCameraLimits(client, cam);
}

void PanoramaScanDialog::setupUi(const PtzCameraEntry &cam,
                                 bool defaultInvertPan,
                                 bool defaultInvertTilt,
                                 FrameCaptureMode defaultCaptureMode)
{
    auto *root = new QVBoxLayout(this);

    const QString camTitle = cam.deviceName.isEmpty() ? cam.sourceName : cam.deviceName;
    auto *header = new QLabel(QStringLiteral("<b>Camara:</b> %1").arg(camTitle), this);
    root->addWidget(header);

    auto *limitsRow = new QHBoxLayout();
    m_limitsLabel = new QLabel(QStringLiteral("Consultando rangos PTZ ONVIF..."), this);
    m_limitsLabel->setWordWrap(true);
    m_applyLimitsBtn = new QPushButton(QStringLiteral("Usar rango completo"), this);
    m_applyLimitsBtn->setEnabled(false);
    limitsRow->addWidget(m_limitsLabel, 1);
    limitsRow->addWidget(m_applyLimitsBtn);
    root->addLayout(limitsRow);

    auto *gridGroup = new QGroupBox(QStringLiteral("Grilla de barrido"), this);
    auto *gridForm = new QFormLayout(gridGroup);

    m_presetCombo = new QComboBox(gridGroup);
    m_presetCombo->addItem(QStringLiteral("Rapida (3 x 2 = 6 tomas)"), QStringLiteral("3x2"));
    m_presetCombo->addItem(QStringLiteral("Estandar (5 x 3 = 15 tomas)"), QStringLiteral("5x3"));
    m_presetCombo->addItem(QStringLiteral("Amplia (7 x 3 = 21 tomas)"), QStringLiteral("7x3"));
    m_presetCombo->addItem(QStringLiteral("Alta densidad (7 x 4 = 28 tomas)"), QStringLiteral("7x4"));
    m_presetCombo->addItem(QStringLiteral("Personalizada"), QStringLiteral("custom"));
    m_presetCombo->setCurrentIndex(1);

    auto *dimsRow = new QHBoxLayout();
    m_colsSpin = new QSpinBox(gridGroup);
    m_colsSpin->setRange(1, 12);
    m_colsSpin->setValue(5);
    m_rowsSpin = new QSpinBox(gridGroup);
    m_rowsSpin->setRange(1, 8);
    m_rowsSpin->setValue(3);
    dimsRow->addWidget(new QLabel(QStringLiteral("Columnas:"), gridGroup));
    dimsRow->addWidget(m_colsSpin);
    dimsRow->addWidget(new QLabel(QStringLiteral("Filas:"), gridGroup));
    dimsRow->addWidget(m_rowsSpin);

    gridForm->addRow(QStringLiteral("Modo:"), m_presetCombo);
    gridForm->addRow(QStringLiteral("Dimensiones:"), dimsRow);
    root->addWidget(gridGroup);

    auto *rangeGroup = new QGroupBox(QStringLiteral("Recorrido y camara"), this);
    auto *rangeForm = new QFormLayout(rangeGroup);

    auto makeCoordSpin = [&](double val, double minV, double maxV) {
        auto *sb = new QDoubleSpinBox(rangeGroup);
        sb->setRange(minV, maxV);
        sb->setDecimals(2);
        sb->setSingleStep(0.05);
        sb->setValue(val);
        return sb;
    };

    auto *panRow = new QHBoxLayout();
    m_panMinSpin = makeCoordSpin(-0.85, -1.0, 1.0);
    m_panMaxSpin = makeCoordSpin(0.85, -1.0, 1.0);
    panRow->addWidget(m_panMinSpin);
    panRow->addWidget(new QLabel(QStringLiteral("a"), rangeGroup));
    panRow->addWidget(m_panMaxSpin);

    auto *tiltRow = new QHBoxLayout();
    m_tiltMinSpin = makeCoordSpin(-0.45, -1.0, 1.0);
    m_tiltMaxSpin = makeCoordSpin(0.45, -1.0, 1.0);
    tiltRow->addWidget(m_tiltMinSpin);
    tiltRow->addWidget(new QLabel(QStringLiteral("a"), rangeGroup));
    tiltRow->addWidget(m_tiltMaxSpin);

    m_scanZoomSpin = makeCoordSpin(0.0, 0.0, 1.0);
    m_settleDelaySpin = new QSpinBox(rangeGroup);
    m_settleDelaySpin->setRange(300, 4000);
    m_settleDelaySpin->setSingleStep(100);
    m_settleDelaySpin->setSuffix(QStringLiteral(" ms"));
    m_settleDelaySpin->setValue(900);

    m_captureModeCombo = new QComboBox(rangeGroup);
    m_captureModeCombo->addItem(
        QStringLiteral("Auto (Snapshot ONVIF -> fallback frame OBS)"),
        static_cast<int>(FrameCaptureMode::AutoOnvifThenObs));
    m_captureModeCombo->addItem(
        QStringLiteral("Solo ONVIF Snapshot HTTP"),
        static_cast<int>(FrameCaptureMode::OnvifSnapshotOnly));
    m_captureModeCombo->addItem(
        QStringLiteral("Solo frame de fuente OBS"),
        static_cast<int>(FrameCaptureMode::ObsSourceOnly));
    const int modeIdx = m_captureModeCombo->findData(static_cast<int>(defaultCaptureMode));
    if (modeIdx >= 0) {
        m_captureModeCombo->setCurrentIndex(modeIdx);
    }

    auto *invRow = new QHBoxLayout();
    m_invertPanCheck = new QCheckBox(QStringLiteral("Invertir Pan"), rangeGroup);
    m_invertPanCheck->setChecked(defaultInvertPan);
    m_invertTiltCheck = new QCheckBox(QStringLiteral("Invertir Tilt"), rangeGroup);
    m_invertTiltCheck->setChecked(defaultInvertTilt);
    invRow->addWidget(m_invertPanCheck);
    invRow->addWidget(m_invertTiltCheck);

    rangeForm->addRow(QStringLiteral("Pan (Izq / Der):"), panRow);
    rangeForm->addRow(QStringLiteral("Tilt (Abajo / Arriba):"), tiltRow);
    rangeForm->addRow(QStringLiteral("Zoom del escaneo:"), m_scanZoomSpin);
    rangeForm->addRow(QStringLiteral("Espera si no reporta estado:"), m_settleDelaySpin);
    rangeForm->addRow(QStringLiteral("Origen de capturas:"), m_captureModeCombo);
    rangeForm->addRow(QStringLiteral("Ejes:"), invRow);
    root->addWidget(rangeGroup);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *startBtn = buttons->addButton(QStringLiteral("Iniciar captura"), QDialogButtonBox::AcceptRole);
    startBtn->setDefault(true);
    buttons->addButton(QStringLiteral("Cancelar"), QDialogButtonBox::RejectRole);
    root->addWidget(buttons);

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PanoramaScanDialog::onGridPresetChanged);
    connect(m_colsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PanoramaScanDialog::onGridSpinChanged);
    connect(m_rowsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PanoramaScanDialog::onGridSpinChanged);
    connect(m_applyLimitsBtn, &QPushButton::clicked,
            this, &PanoramaScanDialog::onApplyDetectedLimits);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PanoramaScanDialog::fetchCameraLimits(OnvifClient *client, const PtzCameraEntry &cam)
{
    if (!client || cam.ptzServiceUrl.isEmpty()) {
        m_limitsLabel->setText(QStringLiteral("Usando rango estandar [-1.0..1.0]."));
        return;
    }

    QPointer<PanoramaScanDialog> self(this);
    client->getPtzLimits(
        cam.ptzServiceUrl, cam.username, cam.password, cam.clockOffsetSecs,
        [self](bool ok, const PtzLimits &lim, const QString &) {
            if (!self) {
                return;
            }
            if (!ok || !lim.hasLimits) {
                self->m_limitsLabel->setText(
                    QStringLiteral("La camara no reporto limites PTZ (usando [-1.0..1.0])."));
                return;
            }
            self->m_detectedLimits = lim;
            self->m_panMinSpin->setRange(lim.panMin, lim.panMax);
            self->m_panMaxSpin->setRange(lim.panMin, lim.panMax);
            self->m_tiltMinSpin->setRange(lim.tiltMin, lim.tiltMax);
            self->m_tiltMaxSpin->setRange(lim.tiltMin, lim.tiltMax);
            self->m_scanZoomSpin->setRange(lim.zoomMin, lim.zoomMax);

            self->m_limitsLabel->setText(
                QStringLiteral("Limites ONVIF detectados: Pan [%1..%2], Tilt [%3..%4]")
                    .arg(lim.panMin, 0, 'f', 2)
                    .arg(lim.panMax, 0, 'f', 2)
                    .arg(lim.tiltMin, 0, 'f', 2)
                    .arg(lim.tiltMax, 0, 'f', 2));
            self->m_applyLimitsBtn->setEnabled(true);
        });
}

void PanoramaScanDialog::onGridPresetChanged(int index)
{
    const QString data = m_presetCombo->itemData(index).toString();
    m_colsSpin->blockSignals(true);
    m_rowsSpin->blockSignals(true);
    if (data == QStringLiteral("3x2")) {
        m_colsSpin->setValue(3);
        m_rowsSpin->setValue(2);
    } else if (data == QStringLiteral("5x3")) {
        m_colsSpin->setValue(5);
        m_rowsSpin->setValue(3);
    } else if (data == QStringLiteral("7x3")) {
        m_colsSpin->setValue(7);
        m_rowsSpin->setValue(3);
    } else if (data == QStringLiteral("7x4")) {
        m_colsSpin->setValue(7);
        m_rowsSpin->setValue(4);
    }
    m_colsSpin->blockSignals(false);
    m_rowsSpin->blockSignals(false);
}

void PanoramaScanDialog::onGridSpinChanged()
{
    const QString cur = QStringLiteral("%1x%2").arg(m_colsSpin->value()).arg(m_rowsSpin->value());
    const int idx = m_presetCombo->findData(cur);
    m_presetCombo->blockSignals(true);
    m_presetCombo->setCurrentIndex(idx >= 0 ? idx : m_presetCombo->findData(QStringLiteral("custom")));
    m_presetCombo->blockSignals(false);
}

void PanoramaScanDialog::onApplyDetectedLimits()
{
    if (!m_detectedLimits.hasLimits) {
        return;
    }
    const double panMid = 0.5 * (m_detectedLimits.panMin + m_detectedLimits.panMax);
    const double panHalf = 0.45 * (m_detectedLimits.panMax - m_detectedLimits.panMin);
    const double tiltMid = 0.5 * (m_detectedLimits.tiltMin + m_detectedLimits.tiltMax);
    const double tiltHalf = 0.40 * (m_detectedLimits.tiltMax - m_detectedLimits.tiltMin);

    m_panMinSpin->setValue(panMid - panHalf);
    m_panMaxSpin->setValue(panMid + panHalf);
    m_tiltMinSpin->setValue(tiltMid - tiltHalf);
    m_tiltMaxSpin->setValue(tiltMid + tiltHalf);
    m_scanZoomSpin->setValue(m_detectedLimits.zoomMin);
}

PanoramaScanSettings PanoramaScanDialog::settings() const
{
    PanoramaScanSettings s;
    s.cols = m_colsSpin->value();
    s.rows = m_rowsSpin->value();
    s.panMin = std::min(m_panMinSpin->value(), m_panMaxSpin->value());
    s.panMax = std::max(m_panMinSpin->value(), m_panMaxSpin->value());
    s.tiltMin = std::min(m_tiltMinSpin->value(), m_tiltMaxSpin->value());
    s.tiltMax = std::max(m_tiltMinSpin->value(), m_tiltMaxSpin->value());
    s.scanZoom = m_scanZoomSpin->value();
    s.settleDelayMs = m_settleDelaySpin->value();
    s.captureMode = static_cast<FrameCaptureMode>(m_captureModeCombo->currentData().toInt());
    s.invertPan = m_invertPanCheck->isChecked();
    s.invertTilt = m_invertTiltCheck->isChecked();
    return s;
}

} // namespace netdev
