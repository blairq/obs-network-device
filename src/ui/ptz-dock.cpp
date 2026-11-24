#include "ptz-dock.hpp"

#include "core/device-registry.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace netdev {

PtzDockWidget::PtzDockWidget(QWidget *parent) : QWidget(parent)
{
    setupUi();

    connect(&DeviceRegistry::instance(), &DeviceRegistry::cameraListChanged,
            this, &PtzDockWidget::reloadCameras);
    connect(&DeviceRegistry::instance(), &DeviceRegistry::activeSourceChanged,
            this, &PtzDockWidget::onActiveSourceChanged);

    reloadCameras();
}

void PtzDockWidget::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto *camRow = new QHBoxLayout();
    m_cameraCombo = new QComboBox(this);
    m_followSceneCheck = new QCheckBox(QStringLiteral("Seguir escena"), this);
    m_followSceneCheck->setChecked(true);
    camRow->addWidget(m_cameraCombo, 1);
    camRow->addWidget(m_followSceneCheck);
    root->addLayout(camRow);

    m_emptyLabel = new QLabel(
        QStringLiteral("Agrega una fuente 'Dispositivo de red' con soporte PTZ para controlarla desde aqui."),
        this);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    root->addWidget(m_emptyLabel);

    m_controlsContainer = new QWidget(this);
    auto *ctrlLayout = new QVBoxLayout(m_controlsContainer);
    ctrlLayout->setContentsMargins(0, 0, 0, 0);
    ctrlLayout->setSpacing(6);

    // 3x3 PTZ Directional Pad
    auto *padGrid = new QGridLayout();
    padGrid->setSpacing(4);
    padGrid->addWidget(makeMoveButton(QStringLiteral("↖"), -1.0, 1.0, 0.0), 0, 0);
    padGrid->addWidget(makeMoveButton(QStringLiteral("↑"), 0.0, 1.0, 0.0), 0, 1);
    padGrid->addWidget(makeMoveButton(QStringLiteral("↗"), 1.0, 1.0, 0.0), 0, 2);
    padGrid->addWidget(makeMoveButton(QStringLiteral("←"), -1.0, 0.0, 0.0), 1, 0);

    auto *homeBtn = new QPushButton(QStringLiteral("⌂"), m_controlsContainer);
    homeBtn->setToolTip(QStringLiteral("Ir a posicion Home"));
    homeBtn->setMinimumHeight(34);
    connect(homeBtn, &QPushButton::clicked, this, &PtzDockWidget::onHomeClicked);
    padGrid->addWidget(homeBtn, 1, 1);

    padGrid->addWidget(makeMoveButton(QStringLiteral("→"), 1.0, 0.0, 0.0), 1, 2);
    padGrid->addWidget(makeMoveButton(QStringLiteral("↙"), -1.0, -1.0, 0.0), 2, 0);
    padGrid->addWidget(makeMoveButton(QStringLiteral("↓"), 0.0, -1.0, 0.0), 2, 1);
    padGrid->addWidget(makeMoveButton(QStringLiteral("↘"), 1.0, -1.0, 0.0), 2, 2);
    ctrlLayout->addLayout(padGrid);

    // Zoom buttons
    auto *zoomRow = new QHBoxLayout();
    auto *zoomOutBtn = makeMoveButton(QStringLiteral("Zoom -"), 0.0, 0.0, -1.0);
    auto *zoomInBtn = makeMoveButton(QStringLiteral("Zoom +"), 0.0, 0.0, 1.0);
    zoomRow->addWidget(zoomOutBtn);
    zoomRow->addWidget(zoomInBtn);
    ctrlLayout->addLayout(zoomRow);

    // Speed slider
    auto *speedRow = new QHBoxLayout();
    m_speedSlider = new QSlider(Qt::Horizontal, m_controlsContainer);
    m_speedSlider->setRange(10, 100);
    m_speedSlider->setValue(50);
    m_speedValueLabel = new QLabel(QStringLiteral("50%"), m_controlsContainer);
    m_speedValueLabel->setMinimumWidth(36);
    speedRow->addWidget(new QLabel(QStringLiteral("Velocidad:"), m_controlsContainer));
    speedRow->addWidget(m_speedSlider, 1);
    speedRow->addWidget(m_speedValueLabel);
    ctrlLayout->addLayout(speedRow);

    // Presets
    auto *presetsGroup = new QGroupBox(QStringLiteral("Presets"), m_controlsContainer);
    auto *presetsLayout = new QVBoxLayout(presetsGroup);
    m_presetsList = new QListWidget(presetsGroup);
    m_presetsList->setMinimumHeight(90);
    presetsLayout->addWidget(m_presetsList, 1);

    auto *presetBtnRow = new QHBoxLayout();
    auto *gotoBtn = new QPushButton(QStringLiteral("Ir"), presetsGroup);
    auto *saveBtn = new QPushButton(QStringLiteral("Guardar"), presetsGroup);
    auto *removeBtn = new QPushButton(QStringLiteral("Borrar"), presetsGroup);
    auto *reloadBtn = new QPushButton(QStringLiteral("↻"), presetsGroup);
    reloadBtn->setToolTip(QStringLiteral("Recargar presets"));
    reloadBtn->setMaximumWidth(32);

    presetBtnRow->addWidget(gotoBtn);
    presetBtnRow->addWidget(saveBtn);
    presetBtnRow->addWidget(removeBtn);
    presetBtnRow->addWidget(reloadBtn);
    presetsLayout->addLayout(presetBtnRow);
    ctrlLayout->addWidget(presetsGroup, 1);

    root->addWidget(m_controlsContainer, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    connect(m_cameraCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PtzDockWidget::onCameraSelectionChanged);
    connect(m_speedSlider, &QSlider::valueChanged, this, &PtzDockWidget::onSpeedChanged);
    connect(gotoBtn, &QPushButton::clicked, this, &PtzDockWidget::onGotoPresetClicked);
    connect(saveBtn, &QPushButton::clicked, this, &PtzDockWidget::onSavePresetClicked);
    connect(removeBtn, &QPushButton::clicked, this, &PtzDockWidget::onRemovePresetClicked);
    connect(reloadBtn, &QPushButton::clicked, this, &PtzDockWidget::onLoadPresetsClicked);
    connect(m_presetsList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { onGotoPresetClicked(); });
}

QPushButton *PtzDockWidget::makeMoveButton(const QString &text,
                                           double panDir,
                                           double tiltDir,
                                           double zoomDir)
{
    auto *btn = new QPushButton(text, m_controlsContainer);
    btn->setAutoRepeat(false);
    btn->setMinimumHeight(34);

    connect(btn, &QPushButton::pressed, this, [this, panDir, tiltDir, zoomDir]() {
        sendMove(panDir, tiltDir, zoomDir);
    });
    connect(btn, &QPushButton::released, this, [this]() {
        sendStop();
    });
    return btn;
}

void PtzDockWidget::reloadCameras()
{
    const QString prevId = m_cameraCombo->currentData().toString();
    const QVector<PtzCameraEntry> cams = DeviceRegistry::instance().cameras();

    m_cameraCombo->blockSignals(true);
    m_cameraCombo->clear();

    int restoreIdx = 0;
    for (int i = 0; i < static_cast<int>(cams.size()); ++i) {
        const PtzCameraEntry &cam = cams.at(i);
        const QString label = cam.sourceName.isEmpty() ? cam.deviceName : cam.sourceName;
        m_cameraCombo->addItem(label, cam.sourceUuid);
        if (cam.sourceUuid == prevId) {
            restoreIdx = i;
        }
    }

    if (!cams.isEmpty()) {
        m_cameraCombo->setCurrentIndex(restoreIdx);
    }
    m_cameraCombo->blockSignals(false);

    const bool hasCams = !cams.isEmpty();
    setControlsEnabled(hasCams);
    if (hasCams) {
        onCameraSelectionChanged();
    } else {
        m_presetsList->clear();
        m_statusLabel->clear();
    }
}

void PtzDockWidget::setControlsEnabled(bool enabled)
{
    m_emptyLabel->setVisible(!enabled);
    m_controlsContainer->setVisible(enabled);
    m_cameraCombo->setEnabled(enabled);
}

bool PtzDockWidget::currentCamera(PtzCameraEntry &out) const
{
    const QString id = m_cameraCombo->currentData().toString();
    if (id.isEmpty()) {
        return false;
    }
    return DeviceRegistry::instance().cameraById(id, out);
}

void PtzDockWidget::onCameraSelectionChanged()
{
    onLoadPresetsClicked();
}

void PtzDockWidget::onActiveSourceChanged(const QString &sourceKey)
{
    if (!m_followSceneCheck->isChecked() || sourceKey.isEmpty()) {
        return;
    }

    PtzCameraEntry entry;
    if (!DeviceRegistry::instance().cameraById(sourceKey, entry) &&
        !DeviceRegistry::instance().findBySourceName(sourceKey, entry)) {
        return;
    }

    for (int i = 0; i < m_cameraCombo->count(); ++i) {
        if (m_cameraCombo->itemData(i).toString() == entry.sourceUuid) {
            if (m_cameraCombo->currentIndex() != i) {
                m_cameraCombo->setCurrentIndex(i);
            }
            break;
        }
    }
}

void PtzDockWidget::onSpeedChanged(int value)
{
    m_speedValueLabel->setText(QStringLiteral("%1%").arg(value));
}

void PtzDockWidget::sendMove(double panDir, double tiltDir, double zoomDir)
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }

    const double speed = static_cast<double>(m_speedSlider->value()) / 100.0;
    QPointer<PtzDockWidget> self(this);
    m_client.continuousMove(
        cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password, cam.clockOffsetSecs,
        panDir * speed, tiltDir * speed, zoomDir * speed,
        [self](bool ok, const QString &err) {
            if (!self) {
                return;
            }
            if (!ok) {
                self->m_statusLabel->setText(QStringLiteral("PTZ: %1").arg(err));
            }
        });
}

void PtzDockWidget::sendStop()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }
    m_client.stopPtz(cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password,
                     cam.clockOffsetSecs, nullptr);
}

void PtzDockWidget::onHomeClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }
    m_statusLabel->setText(QStringLiteral("Moviendo a Home..."));
    QPointer<PtzDockWidget> self(this);
    m_client.gotoHome(cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password,
                      cam.clockOffsetSecs,
                      [self](bool ok, const QString &err) {
                          if (!self) {
                              return;
                          }
                          self->m_statusLabel->setText(ok ? QStringLiteral("Posicion Home enviada.")
                                                          : QStringLiteral("Home fallo: %1").arg(err));
                      });
}

void PtzDockWidget::onLoadPresetsClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }

    m_statusLabel->setText(QStringLiteral("Cargando presets..."));
    QPointer<PtzDockWidget> self(this);
    m_client.getPresets(
        cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password, cam.clockOffsetSecs,
        [self](bool ok, const QVector<PtzPreset> &presets, const QString &err) {
            if (!self) {
                return;
            }
            self->m_presetsList->clear();
            if (!ok) {
                self->m_statusLabel->setText(QStringLiteral("Presets no disponibles: %1").arg(err));
                return;
            }
            for (const PtzPreset &p : presets) {
                auto *item = new QListWidgetItem(p.name, self->m_presetsList);
                item->setData(Qt::UserRole, p.token);
            }
            self->m_statusLabel->setText(QStringLiteral("%1 preset(s) cargado(s).").arg(presets.size()));
        });
}

void PtzDockWidget::onGotoPresetClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }
    QListWidgetItem *item = m_presetsList->currentItem();
    if (!item) {
        return;
    }

    const QString token = item->data(Qt::UserRole).toString();
    m_statusLabel->setText(QStringLiteral("Yendo a '%1'...").arg(item->text()));
    QPointer<PtzDockWidget> self(this);
    m_client.gotoPreset(
        cam.ptzServiceUrl, cam.profileToken, token, cam.username, cam.password, cam.clockOffsetSecs,
        [self](bool ok, const QString &err) {
            if (!self) {
                return;
            }
            self->m_statusLabel->setText(ok ? QStringLiteral("Preset aplicado.")
                                            : QStringLiteral("Error al ir al preset: %1").arg(err));
        });
}

void PtzDockWidget::onSavePresetClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }

    bool okInput = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Guardar preset PTZ"),
        QStringLiteral("Nombre del preset:"), QLineEdit::Normal, QString(), &okInput);
    if (!okInput || name.trimmed().isEmpty()) {
        return;
    }

    QPointer<PtzDockWidget> self(this);
    m_client.setPreset(
        cam.ptzServiceUrl, cam.profileToken, name.trimmed(), cam.username, cam.password,
        cam.clockOffsetSecs,
        [self](bool ok, const QString &err) {
            if (!self) {
                return;
            }
            if (ok) {
                self->m_statusLabel->setText(QStringLiteral("Preset guardado."));
                QTimer::singleShot(300, self.data(), &PtzDockWidget::onLoadPresetsClicked);
            } else {
                self->m_statusLabel->setText(QStringLiteral("Error guardando preset: %1").arg(err));
            }
        });
}

void PtzDockWidget::onRemovePresetClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }
    QListWidgetItem *item = m_presetsList->currentItem();
    if (!item) {
        return;
    }

    const QString token = item->data(Qt::UserRole).toString();
    QPointer<PtzDockWidget> self(this);
    m_client.removePreset(
        cam.ptzServiceUrl, cam.profileToken, token, cam.username, cam.password, cam.clockOffsetSecs,
        [self](bool ok, const QString &err) {
            if (!self) {
                return;
            }
            if (ok) {
                self->m_statusLabel->setText(QStringLiteral("Preset eliminado."));
                QTimer::singleShot(300, self.data(), &PtzDockWidget::onLoadPresetsClicked);
            } else {
                self->m_statusLabel->setText(QStringLiteral("Error borrando preset: %1").arg(err));
            }
        });
}

} // namespace netdev
