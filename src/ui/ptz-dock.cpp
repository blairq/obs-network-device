#include "ptz-dock.hpp"

#include "core/device-registry.hpp"
#include "core/obs-frame-capture.hpp"
#include "ui/panorama-scan-dialog.hpp"
#include "ui/panorama-view-widget.hpp"
#include "ui/virtual-joystick-widget.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace netdev {

namespace {

constexpr int kContinuousMoveThrottleMs = 120;

QString formatPtzSummary(const PtzPosition &pos, bool valid)
{
    if (!valid || !pos.hasPanTilt) {
        return QStringLiteral("Pos: --");
    }
    return QStringLiteral("P: %1  T: %2  Z: %3")
        .arg(pos.pan, 0, 'f', 2)
        .arg(pos.tilt, 0, 'f', 2)
        .arg(pos.zoom, 0, 'f', 2);
}

QIcon makeThumbnailIcon(const QImage &img)
{
    QPixmap pm(120, 68);
    pm.fill(QColor(24, 28, 36));
    if (!img.isNull()) {
        const QImage scaled =
            img.scaled(120, 68, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pm = QPixmap::fromImage(scaled);
    }
    return QIcon(pm);
}

} // namespace

PtzDockWidget::PtzDockWidget(const QString &storageBaseDir, QWidget *parent)
    : QWidget(parent),
      m_store(storageBaseDir),
      m_scanner(&m_client, this),
      m_joystick(this)
{
    m_wheelStopTimer.setSingleShot(true);
    m_wheelStopTimer.setInterval(240);
    connect(&m_wheelStopTimer, &QTimer::timeout, this, &PtzDockWidget::sendStop);

    m_statusRefreshTimer1.setSingleShot(true);
    m_statusRefreshTimer1.setInterval(650);
    connect(&m_statusRefreshTimer1, &QTimer::timeout, this, &PtzDockWidget::queryCurrentStatus);

    m_statusRefreshTimer2.setSingleShot(true);
    m_statusRefreshTimer2.setInterval(1700);
    connect(&m_statusRefreshTimer2, &QTimer::timeout, this, &PtzDockWidget::queryCurrentStatus);

    setupUi();

    connect(&DeviceRegistry::instance(), &DeviceRegistry::cameraListChanged,
            this, &PtzDockWidget::reloadCameras);
    connect(&DeviceRegistry::instance(), &DeviceRegistry::activeSourceChanged,
            this, &PtzDockWidget::onActiveSourceChanged);

    connect(&m_scanner, &PanoramaScanner::progressUpdated,
            this, &PtzDockWidget::onScanProgress);
    connect(&m_scanner, &PanoramaScanner::scanFinished,
            this, &PtzDockWidget::onScanFinished);
    connect(&m_scanner, &PanoramaScanner::scanCancelled,
            this, &PtzDockWidget::onScanCancelled);

    connect(&m_joystick, &JoystickController::axisChanged,
            this, &PtzDockWidget::onPhysicalJoystickAxis);
    connect(&m_joystick, &JoystickController::presetTriggered,
            this, &PtzDockWidget::onPhysicalJoystickPreset);
    connect(&m_joystick, &JoystickController::homeTriggered,
            this, &PtzDockWidget::onHomeClicked);
    connect(&m_joystick, &JoystickController::connectionStatusChanged,
            this, [this](const QString &status) {
                if (m_joystickStatusLabel) {
                    m_joystickStatusLabel->setText(status);
                }
            });

    onRefreshJoysticksClicked();
    reloadCameras();
}

PtzDockWidget::~PtzDockWidget()
{
    if (m_scanner.isRunning()) {
        m_scanner.cancelScan();
    }
    if (m_bigViewDialog) {
        m_bigViewDialog->close();
        delete m_bigViewDialog;
    }
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
    m_homeBtn = new QPushButton(QStringLiteral("⌂"), this);
    m_homeBtn->setToolTip(QStringLiteral("Ir a posicion Home"));
    m_homeBtn->setMaximumWidth(32);
    m_refreshPosBtn = new QPushButton(QStringLiteral("↻"), this);
    m_refreshPosBtn->setToolTip(QStringLiteral("Actualizar posicion PTZ actual"));
    m_refreshPosBtn->setMaximumWidth(32);

    camRow->addWidget(m_cameraCombo, 1);
    camRow->addWidget(m_followSceneCheck);
    camRow->addWidget(m_homeBtn);
    camRow->addWidget(m_refreshPosBtn);
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

    m_tabs = new QTabWidget(m_controlsContainer);
    m_tabs->addTab(createPanoramaTab(m_tabs), QStringLiteral("Panoramica"));
    m_tabs->addTab(createJoystickTab(m_tabs), QStringLiteral("Joystick"));
    m_tabs->addTab(createPresetsTab(m_tabs), QStringLiteral("Presets"));
    ctrlLayout->addWidget(m_tabs, 1);

    root->addWidget(m_controlsContainer, 1);

    auto *footerRow = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_coordsLabel = new QLabel(QStringLiteral("Pos: --"), this);
    m_coordsLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    footerRow->addWidget(m_statusLabel, 1);
    footerRow->addWidget(m_coordsLabel);
    root->addLayout(footerRow);

    connect(m_cameraCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PtzDockWidget::onCameraSelectionChanged);
    connect(m_homeBtn, &QPushButton::clicked, this, &PtzDockWidget::onHomeClicked);
    connect(m_refreshPosBtn, &QPushButton::clicked, this, &PtzDockWidget::onRefreshPositionClicked);
}

QWidget *PtzDockWidget::createPanoramaTab(QWidget *parent)
{
    auto *tab = new QWidget(parent);
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(4, 6, 4, 4);
    layout->setSpacing(6);

    m_panoramaView = new PanoramaViewWidget(tab);
    layout->addWidget(m_panoramaView, 1);

    m_scanPanel = new QWidget(tab);
    m_scanPanel->setVisible(false);
    auto *scanLayout = new QHBoxLayout(m_scanPanel);
    scanLayout->setContentsMargins(0, 0, 0, 0);
    scanLayout->setSpacing(6);

    m_scanThumbLabel = new QLabel(m_scanPanel);
    m_scanThumbLabel->setFixedSize(64, 36);
    m_scanThumbLabel->setScaledContents(true);
    m_scanThumbLabel->setStyleSheet(QStringLiteral("background: #111827; border: 1px solid #374151;"));

    auto *scanMidCol = new QVBoxLayout();
    m_scanStatusLabel = new QLabel(QStringLiteral("Escaneando..."), m_scanPanel);
    m_scanProgress = new QProgressBar(m_scanPanel);
    m_scanProgress->setRange(0, 15);
    m_scanProgress->setMaximumHeight(10);
    scanMidCol->addWidget(m_scanStatusLabel);
    scanMidCol->addWidget(m_scanProgress);

    m_cancelScanBtn = new QPushButton(QStringLiteral("Cancelar"), m_scanPanel);
    scanLayout->addWidget(m_scanThumbLabel);
    scanLayout->addLayout(scanMidCol, 1);
    scanLayout->addWidget(m_cancelScanBtn);
    layout->addWidget(m_scanPanel);

    auto *actionRow = new QHBoxLayout();
    m_capturePanoBtn = new QPushButton(QStringLiteral("Capturar panoramica..."), tab);
    auto *savePointBtn = new QPushButton(QStringLiteral("Guardar punto actual"), tab);
    m_bigViewBtn = new QPushButton(QStringLiteral("Ventana grande"), tab);
    actionRow->addWidget(m_capturePanoBtn);
    actionRow->addWidget(savePointBtn);
    actionRow->addWidget(m_bigViewBtn);
    layout->addLayout(actionRow);

    auto *zoomRow = new QHBoxLayout();
    zoomRow->addWidget(makeMoveButton(QStringLiteral("Zoom -"), -1.0));
    zoomRow->addWidget(makeMoveButton(QStringLiteral("Zoom +"), 1.0));
    layout->addLayout(zoomRow);

    connect(m_panoramaView, &PanoramaViewWidget::pointClicked,
            this, &PtzDockWidget::onPanoramaPointClicked);
    connect(m_panoramaView, &PanoramaViewWidget::presetPinClicked,
            this, &PtzDockWidget::onPanoramaPresetPinClicked);
    connect(m_panoramaView, &PanoramaViewWidget::savePresetRequested,
            this, &PtzDockWidget::onSaveVisualPresetClicked);
    connect(m_panoramaView, &PanoramaViewWidget::zoomRequested,
            this, &PtzDockWidget::onPanoramaZoomRequested);

    connect(m_capturePanoBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onCapturePanoramaClicked);
    connect(m_cancelScanBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onCancelScanClicked);
    connect(savePointBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onSaveVisualPresetClicked);
    connect(m_bigViewBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onOpenBigPanoramaClicked);

    return tab;
}

QWidget *PtzDockWidget::createJoystickTab(QWidget *parent)
{
    auto *tab = new QWidget(parent);
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(4, 6, 4, 4);
    layout->setSpacing(6);

    auto *joyRow = new QHBoxLayout();
    m_virtualJoystick = new VirtualJoystickWidget(tab);
    joyRow->addWidget(m_virtualJoystick, 1);

    auto *sideCol = new QVBoxLayout();
    sideCol->addWidget(makeMoveButton(QStringLiteral("Zoom +"), 1.0));
    sideCol->addWidget(makeMoveButton(QStringLiteral("Zoom -"), -1.0));
    auto *homeBtn2 = new QPushButton(QStringLiteral("Home"), tab);
    connect(homeBtn2, &QPushButton::clicked, this, &PtzDockWidget::onHomeClicked);
    sideCol->addWidget(homeBtn2);
    sideCol->addStretch();
    joyRow->addLayout(sideCol);
    layout->addLayout(joyRow);

    auto *speedRow = new QHBoxLayout();
    m_speedSlider = new QSlider(Qt::Horizontal, tab);
    m_speedSlider->setRange(10, 100);
    m_speedSlider->setValue(55);
    m_speedValueLabel = new QLabel(QStringLiteral("55%"), tab);
    m_speedValueLabel->setMinimumWidth(36);
    speedRow->addWidget(new QLabel(QStringLiteral("Velocidad:"), tab));
    speedRow->addWidget(m_speedSlider, 1);
    speedRow->addWidget(m_speedValueLabel);
    layout->addLayout(speedRow);

    auto *invRow = new QHBoxLayout();
    m_invertPanCheck = new QCheckBox(QStringLiteral("Invertir Pan"), tab);
    m_invertTiltCheck = new QCheckBox(QStringLiteral("Invertir Tilt"), tab);
    invRow->addWidget(m_invertPanCheck);
    invRow->addWidget(m_invertTiltCheck);
    invRow->addStretch();
    layout->addLayout(invRow);

    auto *hwGroup = new QGroupBox(QStringLiteral("Joystick fisico USB / Gamepad"), tab);
    auto *hwLayout = new QVBoxLayout(hwGroup);
    auto *devRow = new QHBoxLayout();
    m_joystickCombo = new QComboBox(hwGroup);
    auto *refreshJoyBtn = new QPushButton(QStringLiteral("↻"), hwGroup);
    refreshJoyBtn->setMaximumWidth(32);
    refreshJoyBtn->setToolTip(QStringLiteral("Buscar joysticks o gamepads conectados"));
    devRow->addWidget(m_joystickCombo, 1);
    devRow->addWidget(refreshJoyBtn);
    hwLayout->addLayout(devRow);

    m_joystickStatusLabel = new QLabel(QStringLiteral("Joystick fisico desactivado"), hwGroup);
    m_joystickStatusLabel->setWordWrap(true);
    hwLayout->addWidget(m_joystickStatusLabel);
    layout->addWidget(hwGroup);

    connect(m_virtualJoystick, &VirtualJoystickWidget::moved,
            this, &PtzDockWidget::onVirtualJoystickMoved);
    connect(m_virtualJoystick, &VirtualJoystickWidget::released,
            this, &PtzDockWidget::onVirtualJoystickReleased);
    connect(m_speedSlider, &QSlider::valueChanged,
            this, &PtzDockWidget::onSpeedChanged);
    connect(m_invertPanCheck, &QCheckBox::toggled,
            this, &PtzDockWidget::onInvertAxisToggled);
    connect(m_invertTiltCheck, &QCheckBox::toggled,
            this, &PtzDockWidget::onInvertAxisToggled);
    connect(m_joystickCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PtzDockWidget::onJoystickDeviceChanged);
    connect(refreshJoyBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onRefreshJoysticksClicked);

    return tab;
}

QWidget *PtzDockWidget::createPresetsTab(QWidget *parent)
{
    auto *tab = new QWidget(parent);
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(4, 6, 4, 4);
    layout->setSpacing(6);

    auto *visualGroup = new QGroupBox(QStringLiteral("Vistas guardadas con imagen y PTZ"), tab);
    auto *visualLayout = new QVBoxLayout(visualGroup);

    m_visualPresetsList = new QListWidget(visualGroup);
    m_visualPresetsList->setViewMode(QListView::IconMode);
    m_visualPresetsList->setResizeMode(QListView::Adjust);
    m_visualPresetsList->setMovement(QListView::Static);
    m_visualPresetsList->setWordWrap(true);
    m_visualPresetsList->setIconSize(QSize(120, 68));
    m_visualPresetsList->setGridSize(QSize(134, 108));
    m_visualPresetsList->setMinimumHeight(130);
    visualLayout->addWidget(m_visualPresetsList, 1);

    auto *visBtnRow1 = new QHBoxLayout();
    auto *saveVisBtn = new QPushButton(QStringLiteral("+ Guardar vista"), visualGroup);
    auto *gotoVisBtn = new QPushButton(QStringLiteral("Ir"), visualGroup);
    auto *photoBtn = new QPushButton(QStringLiteral("Actualizar foto"), visualGroup);
    auto *renBtn = new QPushButton(QStringLiteral("Renombrar"), visualGroup);
    auto *delVisBtn = new QPushButton(QStringLiteral("Borrar"), visualGroup);
    visBtnRow1->addWidget(saveVisBtn);
    visBtnRow1->addWidget(gotoVisBtn);
    visBtnRow1->addWidget(photoBtn);
    visBtnRow1->addWidget(renBtn);
    visBtnRow1->addWidget(delVisBtn);
    visualLayout->addLayout(visBtnRow1);
    layout->addWidget(visualGroup, 2);

    auto *camPresetsGroup = new QGroupBox(QStringLiteral("Presets internos de la camara (ONVIF)"), tab);
    auto *camPresetsLayout = new QVBoxLayout(camPresetsGroup);
    m_presetsList = new QListWidget(camPresetsGroup);
    m_presetsList->setMaximumHeight(95);
    camPresetsLayout->addWidget(m_presetsList);

    auto *presetBtnRow = new QHBoxLayout();
    auto *gotoBtn = new QPushButton(QStringLiteral("Ir"), camPresetsGroup);
    auto *saveBtn = new QPushButton(QStringLiteral("Guardar"), camPresetsGroup);
    auto *removeBtn = new QPushButton(QStringLiteral("Borrar"), camPresetsGroup);
    auto *reloadBtn = new QPushButton(QStringLiteral("↻"), camPresetsGroup);
    reloadBtn->setToolTip(QStringLiteral("Recargar presets ONVIF"));
    reloadBtn->setMaximumWidth(32);
    presetBtnRow->addWidget(gotoBtn);
    presetBtnRow->addWidget(saveBtn);
    presetBtnRow->addWidget(removeBtn);
    presetBtnRow->addWidget(reloadBtn);
    camPresetsLayout->addLayout(presetBtnRow);
    layout->addWidget(camPresetsGroup, 1);

    connect(m_visualPresetsList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { onGotoVisualPresetClicked(); });
    connect(saveVisBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onSaveVisualPresetClicked);
    connect(gotoVisBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onGotoVisualPresetClicked);
    connect(photoBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onUpdateVisualPresetPhotoClicked);
    connect(renBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onRenameVisualPresetClicked);
    connect(delVisBtn, &QPushButton::clicked,
            this, &PtzDockWidget::onDeleteVisualPresetClicked);

    connect(gotoBtn, &QPushButton::clicked, this, &PtzDockWidget::onGotoPresetClicked);
    connect(saveBtn, &QPushButton::clicked, this, &PtzDockWidget::onSavePresetClicked);
    connect(removeBtn, &QPushButton::clicked, this, &PtzDockWidget::onRemovePresetClicked);
    connect(reloadBtn, &QPushButton::clicked, this, &PtzDockWidget::onLoadPresetsClicked);
    connect(m_presetsList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { onGotoPresetClicked(); });

    return tab;
}

QPushButton *PtzDockWidget::makeMoveButton(const QString &text, double zoomDir)
{
    auto *btn = new QPushButton(text, m_controlsContainer);
    btn->setAutoRepeat(false);
    btn->setMinimumHeight(30);

    connect(btn, &QPushButton::pressed, this, [this, zoomDir]() {
        m_btnZoomDir = zoomDir;
        updateContinuousMotion(true);
    });
    connect(btn, &QPushButton::released, this, [this]() {
        m_btnZoomDir = 0.0;
        updateContinuousMotion(true);
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
        m_activeProfile = CameraPtzProfile{};
        m_activePanoramaImage = QImage();
        m_hasCurrentPos = false;
        m_hasTarget = false;
        syncPanoramaViews();
        if (m_visualPresetsList) {
            m_visualPresetsList->clear();
        }
        m_presetsList->clear();
        m_statusLabel->clear();
        m_coordsLabel->setText(QStringLiteral("Pos: --"));
    }
}

void PtzDockWidget::setControlsEnabled(bool enabled)
{
    m_emptyLabel->setVisible(!enabled);
    m_controlsContainer->setVisible(enabled);
    m_cameraCombo->setEnabled(enabled);
    m_homeBtn->setEnabled(enabled);
    m_refreshPosBtn->setEnabled(enabled);
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
    if (m_scanner.isRunning()) {
        m_scanner.cancelScan();
    }
    m_hasCurrentPos = false;
    m_hasTarget = false;
    m_coordsLabel->setText(QStringLiteral("Pos: --"));

    loadCurrentCameraProfile();
    onLoadPresetsClicked();
    queryCurrentStatus();
}

void PtzDockWidget::loadCurrentCameraProfile()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }

    m_activeProfile = CameraPtzProfile{};
    m_activePanoramaImage = QImage();

    const bool loaded = m_store.loadProfile(
        cam.cameraKey, cam.fallbackHostKey, m_activeProfile, m_activePanoramaImage);

    if (!loaded || m_activeProfile.cameraKey.isEmpty()) {
        m_activeProfile.cameraKey = cam.cameraKey;
    }
    m_activeProfile.cameraName = cam.deviceName.isEmpty() ? cam.sourceName : cam.deviceName;
    if (!cam.manufacturer.isEmpty()) {
        m_activeProfile.manufacturer = cam.manufacturer;
    }
    if (!cam.model.isEmpty()) {
        m_activeProfile.model = cam.model;
    }
    if (!cam.serialNumber.isEmpty()) {
        m_activeProfile.serialNumber = cam.serialNumber;
    }
    m_activeProfile.deviceServiceUrl = cam.deviceServiceUrl;
    m_activeProfile.ptzServiceUrl = cam.ptzServiceUrl;

    if (m_invertPanCheck && m_invertTiltCheck) {
        m_invertPanCheck->blockSignals(true);
        m_invertTiltCheck->blockSignals(true);
        m_invertPanCheck->setChecked(m_activeProfile.invertPan);
        m_invertTiltCheck->setChecked(m_activeProfile.invertTilt);
        m_invertPanCheck->blockSignals(false);
        m_invertTiltCheck->blockSignals(false);
    }

    refreshVisualPresetsList();
    syncPanoramaViews();
}

void PtzDockWidget::refreshVisualPresetsList()
{
    if (!m_visualPresetsList) {
        return;
    }
    const QString prevSelectedId =
        m_visualPresetsList->currentItem()
            ? m_visualPresetsList->currentItem()->data(Qt::UserRole).toString()
            : QString();

    m_visualPresetsList->clear();
    for (int i = 0; i < static_cast<int>(m_activeProfile.savedPresets.size()); ++i) {
        const SavedPtzPreset &p = m_activeProfile.savedPresets.at(i);
        const QImage thumb =
            m_store.loadPresetThumbnail(m_activeProfile.cameraKey, p.thumbnailFile);
        const QString displayText = QStringLiteral("%1. %2").arg(i + 1).arg(p.name);

        auto *item = new QListWidgetItem(makeThumbnailIcon(thumb), displayText, m_visualPresetsList);
        item->setData(Qt::UserRole, p.id);
        item->setToolTip(
            p.hasPosition
                ? QStringLiteral("%1\nPan: %2  Tilt: %3  Zoom: %4")
                      .arg(p.name)
                      .arg(p.pan, 0, 'f', 2)
                      .arg(p.tilt, 0, 'f', 2)
                      .arg(p.zoom, 0, 'f', 2)
                : p.name);
        if (p.id == prevSelectedId) {
            m_visualPresetsList->setCurrentItem(item);
        }
    }

    syncPanoramaViews();
}

void PtzDockWidget::syncPanoramaViews()
{
    const auto applyToView = [this](PanoramaViewWidget *view) {
        if (!view) {
            return;
        }
        if (!m_activePanoramaImage.isNull() && m_activeProfile.panorama.isValid()) {
            view->setPanorama(m_activePanoramaImage, m_activeProfile.panorama);
        } else {
            view->clearPanorama();
        }
        view->setSavedPresets(m_activeProfile.savedPresets);
        view->setCurrentPosition(m_currentPos, m_hasCurrentPos);
        view->setTargetPosition(m_targetPan, m_targetTilt, m_hasTarget);
    };

    applyToView(m_panoramaView);
    if (m_bigPanoramaView) {
        applyToView(m_bigPanoramaView.data());
    }
}

void PtzDockWidget::selectCameraBySourceUuid(const QString &sourceUuid)
{
    if (sourceUuid.isEmpty() || m_scanner.isRunning()) {
        return;
    }

    auto findIndex = [this, &sourceUuid]() -> int {
        for (int i = 0; i < m_cameraCombo->count(); ++i) {
            if (m_cameraCombo->itemData(i).toString() == sourceUuid) {
                return i;
            }
        }
        return -1;
    };

    int idx = findIndex();
    if (idx < 0) {
        reloadCameras();
        idx = findIndex();
    }
    if (idx >= 0 && m_cameraCombo->currentIndex() != idx) {
        m_cameraCombo->setCurrentIndex(idx);
    }
}

void PtzDockWidget::onActiveSourceChanged(const QString &sourceKey)
{
    if (!m_followSceneCheck->isChecked() || sourceKey.isEmpty() || m_scanner.isRunning()) {
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
                          self->m_statusLabel->setText(
                              ok ? QStringLiteral("Posicion Home enviada.")
                                 : QStringLiteral("Home fallo: %1").arg(err));
                          if (ok) {
                              self->scheduleStatusRefresh();
                          }
                      });
}

void PtzDockWidget::onRefreshPositionClicked()
{
    queryCurrentStatus();
}

void PtzDockWidget::scheduleStatusRefresh()
{
    m_statusRefreshTimer1.start();
    m_statusRefreshTimer2.start();
}

void PtzDockWidget::queryCurrentStatus()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam) || m_scanner.isRunning()) {
        return;
    }

    const QString camUuid = cam.sourceUuid;
    QPointer<PtzDockWidget> self(this);
    m_client.getPtzStatus(
        cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password, cam.clockOffsetSecs,
        [self, camUuid](bool ok, const PtzStatus &st, const QString &) {
            if (!self) {
                return;
            }
            PtzCameraEntry curCam;
            if (!self->currentCamera(curCam) || curCam.sourceUuid != camUuid) {
                return;
            }
            if (ok && st.valid && st.position.hasPanTilt) {
                self->m_currentPos = st.position;
                self->m_hasCurrentPos = true;
                self->m_coordsLabel->setText(formatPtzSummary(st.position, true));
                self->syncPanoramaViews();
            }
        });
}

void PtzDockWidget::onCapturePanoramaClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }

    PanoramaScanDialog dlg(&m_client, cam, m_activeProfile.invertPan, m_activeProfile.invertTilt,
                           FrameCaptureMode::AutoOnvifThenObs, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const PanoramaScanSettings scanCfg = dlg.settings();
    if (m_invertPanCheck && m_invertTiltCheck) {
        m_invertPanCheck->blockSignals(true);
        m_invertTiltCheck->blockSignals(true);
        m_invertPanCheck->setChecked(scanCfg.invertPan);
        m_invertTiltCheck->setChecked(scanCfg.invertTilt);
        m_invertPanCheck->blockSignals(false);
        m_invertTiltCheck->blockSignals(false);
    }
    m_activeProfile.invertPan = scanCfg.invertPan;
    m_activeProfile.invertTilt = scanCfg.invertTilt;
    m_store.saveProfile(m_activeProfile);

    m_scanProgress->setRange(0, std::max(1, scanCfg.cols * scanCfg.rows));
    m_scanProgress->setValue(0);
    m_scanThumbLabel->clear();
    m_scanStatusLabel->setText(QStringLiteral("Iniciando captura..."));
    m_scanPanel->setVisible(true);
    m_capturePanoBtn->setEnabled(false);

    m_scanner.startScan(cam, scanCfg);
}

void PtzDockWidget::onCancelScanClicked()
{
    m_scanner.cancelScan();
}

void PtzDockWidget::onScanProgress(int completed,
                                   int total,
                                   const QString &statusText,
                                   const QImage &lastTile)
{
    m_scanProgress->setRange(0, std::max(1, total));
    m_scanProgress->setValue(completed);
    m_scanStatusLabel->setText(statusText);
    if (!lastTile.isNull()) {
        m_scanThumbLabel->setPixmap(QPixmap::fromImage(
            lastTile.scaled(64, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
}

void PtzDockWidget::onScanFinished(bool ok,
                                   const QImage &panorama,
                                   const PanoramaMeta &meta,
                                   const QString &error)
{
    m_scanPanel->setVisible(false);
    m_capturePanoBtn->setEnabled(true);

    if (!ok || panorama.isNull()) {
        m_statusLabel->setText(QStringLiteral("Panoramica fallo: %1").arg(error));
        return;
    }

    m_activePanoramaImage = panorama;
    m_hasTarget = false;
    if (!m_store.savePanorama(m_activeProfile, panorama, meta)) {
        m_statusLabel->setText(QStringLiteral("Panoramica creada, pero fallo el guardado en disco."));
    } else {
        m_statusLabel->setText(
            QStringLiteral("Panoramica guardada (%1x%2 px).")
                .arg(panorama.width())
                .arg(panorama.height()));
    }
    syncPanoramaViews();
    scheduleStatusRefresh();
}

void PtzDockWidget::onScanCancelled()
{
    m_scanPanel->setVisible(false);
    m_capturePanoBtn->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("Captura de panoramica cancelada."));
}

void PtzDockWidget::onPanoramaPointClicked(double pan, double tilt)
{
    PtzCameraEntry cam;
    if (!currentCamera(cam) || m_scanner.isRunning()) {
        return;
    }

    m_targetPan = pan;
    m_targetTilt = tilt;
    m_hasTarget = true;
    syncPanoramaViews();

    const bool useZoom = m_hasCurrentPos && m_currentPos.hasZoom;
    const double targetZoom = useZoom ? m_currentPos.zoom : m_activeProfile.panorama.scanZoom;
    m_statusLabel->setText(
        QStringLiteral("Moviendo a P=%1, T=%2...").arg(pan, 0, 'f', 2).arg(tilt, 0, 'f', 2));

    QPointer<PtzDockWidget> self(this);
    m_client.absoluteMove(
        cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password, cam.clockOffsetSecs,
        pan, tilt, targetZoom, useZoom, 0.85,
        [self, cam, pan, tilt](bool ok, const QString &err) {
            if (!self) {
                return;
            }
            if (!ok) {
                self->m_client.absoluteMove(
                    cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password,
                    cam.clockOffsetSecs, pan, tilt, 0.0, false, 0.85,
                    [self, err](bool ok2, const QString &err2) {
                        if (!self) {
                            return;
                        }
                        self->m_statusLabel->setText(
                            ok2 ? QStringLiteral("Camara movida al punto.")
                                : QStringLiteral("AbsoluteMove fallo: %1")
                                      .arg(err2.isEmpty() ? err : err2));
                        if (ok2) {
                            self->scheduleStatusRefresh();
                        }
                    });
                return;
            }
            self->m_statusLabel->setText(QStringLiteral("Camara movida al punto."));
            self->scheduleStatusRefresh();
        });
}

void PtzDockWidget::onPanoramaPresetPinClicked(const QString &presetId)
{
    for (const SavedPtzPreset &p : m_activeProfile.savedPresets) {
        if (p.id == presetId) {
            recallSavedPreset(p);
            return;
        }
    }
}

void PtzDockWidget::onPanoramaZoomRequested(int deltaSteps)
{
    PtzCameraEntry cam;
    if (!currentCamera(cam) || m_scanner.isRunning()) {
        return;
    }
    const double dir = (deltaSteps >= 0) ? 0.5 : -0.5;
    sendMove(0.0, 0.0, dir);
    m_wheelStopTimer.start();
}

void PtzDockWidget::onOpenBigPanoramaClicked()
{
    if (m_bigViewDialog) {
        m_bigViewDialog->show();
        m_bigViewDialog->raise();
        m_bigViewDialog->activateWindow();
        syncPanoramaViews();
        return;
    }

    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(QStringLiteral("Mapa Panoramico PTZ"));
    dlg->resize(920, 520);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);

    auto *layout = new QVBoxLayout(dlg);
    auto *bigView = new PanoramaViewWidget(dlg);
    bigView->setMinimumHeight(340);
    layout->addWidget(bigView, 1);

    auto *bar = new QHBoxLayout();
    auto *saveBtn = new QPushButton(QStringLiteral("Guardar punto actual"), dlg);
    auto *homeBtn = new QPushButton(QStringLiteral("Home"), dlg);
    auto *zoomOutBtn = new QPushButton(QStringLiteral("Zoom -"), dlg);
    auto *zoomInBtn = new QPushButton(QStringLiteral("Zoom +"), dlg);
    auto *closeBtn = new QPushButton(QStringLiteral("Cerrar"), dlg);
    bar->addWidget(saveBtn);
    bar->addWidget(homeBtn);
    bar->addStretch();
    bar->addWidget(zoomOutBtn);
    bar->addWidget(zoomInBtn);
    bar->addWidget(closeBtn);
    layout->addLayout(bar);

    connect(bigView, &PanoramaViewWidget::pointClicked,
            this, &PtzDockWidget::onPanoramaPointClicked);
    connect(bigView, &PanoramaViewWidget::presetPinClicked,
            this, &PtzDockWidget::onPanoramaPresetPinClicked);
    connect(bigView, &PanoramaViewWidget::savePresetRequested,
            this, &PtzDockWidget::onSaveVisualPresetClicked);
    connect(bigView, &PanoramaViewWidget::zoomRequested,
            this, &PtzDockWidget::onPanoramaZoomRequested);

    connect(saveBtn, &QPushButton::clicked, this, &PtzDockWidget::onSaveVisualPresetClicked);
    connect(homeBtn, &QPushButton::clicked, this, &PtzDockWidget::onHomeClicked);
    connect(zoomOutBtn, &QPushButton::pressed, this, [this]() {
        m_btnZoomDir = -1.0;
        updateContinuousMotion(true);
    });
    connect(zoomOutBtn, &QPushButton::released, this, [this]() {
        m_btnZoomDir = 0.0;
        updateContinuousMotion(true);
    });
    connect(zoomInBtn, &QPushButton::pressed, this, [this]() {
        m_btnZoomDir = 1.0;
        updateContinuousMotion(true);
    });
    connect(zoomInBtn, &QPushButton::released, this, [this]() {
        m_btnZoomDir = 0.0;
        updateContinuousMotion(true);
    });
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);

    m_bigViewDialog = dlg;
    m_bigPanoramaView = bigView;
    syncPanoramaViews();
    dlg->show();
}

void PtzDockWidget::onVirtualJoystickMoved(double x, double y)
{
    m_vJoyPan = x;
    m_vJoyTilt = y;
    updateContinuousMotion(false);
}

void PtzDockWidget::onVirtualJoystickReleased()
{
    m_vJoyPan = 0.0;
    m_vJoyTilt = 0.0;
    updateContinuousMotion(true);
}

void PtzDockWidget::onRefreshJoysticksClicked()
{
    if (!m_joystickCombo) {
        return;
    }
    const QString prevId = m_joystickCombo->currentData().toString();
    const QVector<JoystickDeviceInfo> devs = m_joystick.enumerateDevices();

    m_joystickCombo->blockSignals(true);
    m_joystickCombo->clear();
    m_joystickCombo->addItem(QStringLiteral("Desactivado"), QString());
    m_joystickCombo->addItem(QStringLiteral("Auto (primer joystick/gamepad)"), QStringLiteral("auto"));
    for (const JoystickDeviceInfo &d : devs) {
        m_joystickCombo->addItem(d.name, d.id);
    }
    const int restoreIdx = m_joystickCombo->findData(prevId);
    m_joystickCombo->setCurrentIndex(restoreIdx >= 0 ? restoreIdx : 0);
    m_joystickCombo->blockSignals(false);
    onJoystickDeviceChanged();
}

void PtzDockWidget::onJoystickDeviceChanged()
{
    if (!m_joystickCombo) {
        return;
    }
    m_joystick.setSelectedDeviceId(m_joystickCombo->currentData().toString());
}

void PtzDockWidget::onPhysicalJoystickAxis(double pan, double tilt, double zoom)
{
    m_pJoyPan = pan;
    m_pJoyTilt = tilt;
    m_pJoyZoom = zoom;
    updateContinuousMotion(false);
}

void PtzDockWidget::onPhysicalJoystickPreset(int index)
{
    if (index >= 0 && index < static_cast<int>(m_activeProfile.savedPresets.size())) {
        recallSavedPreset(m_activeProfile.savedPresets.at(index));
    }
}

void PtzDockWidget::onInvertAxisToggled()
{
    m_activeProfile.invertPan = m_invertPanCheck->isChecked();
    m_activeProfile.invertTilt = m_invertTiltCheck->isChecked();
    m_store.saveProfile(m_activeProfile);
}

void PtzDockWidget::updateContinuousMotion(bool forceImmediate)
{
    if (m_scanner.isRunning()) {
        return;
    }

    const double pan = std::clamp(m_vJoyPan + m_pJoyPan, -1.0, 1.0);
    const double tilt = std::clamp(m_vJoyTilt + m_pJoyTilt, -1.0, 1.0);
    const double zoom = std::clamp(m_pJoyZoom + m_btnZoomDir, -1.0, 1.0);
    const bool moving = (std::abs(pan) > 1e-3) || (std::abs(tilt) > 1e-3) ||
                        (std::abs(zoom) > 1e-3);

    if (!moving) {
        if (m_isMovingContinuous) {
            m_isMovingContinuous = false;
            sendStop();
            scheduleStatusRefresh();
        }
        return;
    }

    if (!forceImmediate && m_isMovingContinuous && m_moveThrottleTimer.isValid() &&
        m_moveThrottleTimer.elapsed() < kContinuousMoveThrottleMs) {
        return;
    }

    m_isMovingContinuous = true;
    m_moveThrottleTimer.restart();
    sendMove(pan, tilt, zoom);
}

void PtzDockWidget::sendMove(double panDir, double tiltDir, double zoomDir)
{
    PtzCameraEntry cam;
    if (!currentCamera(cam)) {
        return;
    }

    const double speed = static_cast<double>(m_speedSlider->value()) / 100.0;
    const double panSign = (m_invertPanCheck && m_invertPanCheck->isChecked()) ? -1.0 : 1.0;
    const double tiltSign = (m_invertTiltCheck && m_invertTiltCheck->isChecked()) ? -1.0 : 1.0;

    QPointer<PtzDockWidget> self(this);
    m_client.continuousMove(
        cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password, cam.clockOffsetSecs,
        panDir * speed * panSign, tiltDir * speed * tiltSign, zoomDir * speed,
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

void PtzDockWidget::captureCurrentFrameForCamera(const PtzCameraEntry &cam,
                                                 std::function<void(const QImage &)> cb)
{
    if (!cb) {
        return;
    }
    QPointer<PtzDockWidget> self(this);
    auto obsFallback = [self, sourceUuid = cam.sourceUuid, cb]() {
        if (!self) {
            return;
        }
        captureObsSourceFrameAsync(sourceUuid, self.data(), cb);
    };

    if (!cam.snapshotUri.trimmed().isEmpty()) {
        m_client.fetchSnapshotToCallback(
            cam.snapshotUri, cam.username, cam.password,
            [self, obsFallback, cb](bool ok, const QImage &image, const QString &) {
                if (!self) {
                    return;
                }
                if (ok && !image.isNull()) {
                    cb(image);
                } else {
                    obsFallback();
                }
            });
        return;
    }

    obsFallback();
}

void PtzDockWidget::onSaveVisualPresetClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam) || m_scanner.isRunning()) {
        return;
    }

    bool okInput = false;
    const QString defaultName =
        QStringLiteral("Vista %1").arg(m_activeProfile.savedPresets.size() + 1);
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Guardar preset visual PTZ"),
        QStringLiteral("Nombre de la vista:"), QLineEdit::Normal, defaultName, &okInput);
    if (!okInput || name.trimmed().isEmpty()) {
        return;
    }

    const QString trimmedName = name.trimmed();
    m_statusLabel->setText(QStringLiteral("Capturando posicion e imagen para '%1'...").arg(trimmedName));

    QPointer<PtzDockWidget> self(this);
    m_client.getPtzStatus(
        cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password, cam.clockOffsetSecs,
        [self, cam, trimmedName](bool okStatus, const PtzStatus &st, const QString &) {
            if (!self) {
                return;
            }
            self->captureCurrentFrameForCamera(
                cam,
                [self, cam, trimmedName, okStatus, st](const QImage &img) {
                    if (!self) {
                        return;
                    }
                    PtzCameraEntry curCam;
                    if (!self->currentCamera(curCam) || curCam.sourceUuid != cam.sourceUuid) {
                        return;
                    }

                    SavedPtzPreset preset;
                    preset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    preset.name = trimmedName;
                    preset.createdAtIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                    if (okStatus && st.valid && st.position.hasPanTilt) {
                        preset.hasPosition = true;
                        preset.pan = st.position.pan;
                        preset.tilt = st.position.tilt;
                        preset.zoom = st.position.hasZoom ? st.position.zoom : 0.0;
                    } else if (self->m_hasCurrentPos) {
                        preset.hasPosition = true;
                        preset.pan = self->m_currentPos.pan;
                        preset.tilt = self->m_currentPos.tilt;
                        preset.zoom = self->m_currentPos.zoom;
                    }
                    if (!img.isNull()) {
                        preset.thumbnailFile = self->m_store.savePresetThumbnail(
                            self->m_activeProfile.cameraKey, preset.id, img);
                    }

                    self->m_activeProfile.savedPresets.push_back(preset);
                    self->m_store.saveProfile(self->m_activeProfile);
                    self->refreshVisualPresetsList();
                    self->m_statusLabel->setText(
                        QStringLiteral("Vista '%1' guardada.").arg(trimmedName));
                });
        });
}

void PtzDockWidget::onGotoVisualPresetClicked()
{
    if (!m_visualPresetsList) {
        return;
    }
    QListWidgetItem *item = m_visualPresetsList->currentItem();
    if (!item) {
        return;
    }
    const QString id = item->data(Qt::UserRole).toString();
    for (const SavedPtzPreset &p : m_activeProfile.savedPresets) {
        if (p.id == id) {
            recallSavedPreset(p);
            return;
        }
    }
}

void PtzDockWidget::recallSavedPreset(const SavedPtzPreset &preset)
{
    PtzCameraEntry cam;
    if (!currentCamera(cam) || m_scanner.isRunning()) {
        return;
    }

    if (preset.hasPosition) {
        m_targetPan = preset.pan;
        m_targetTilt = preset.tilt;
        m_hasTarget = true;
        syncPanoramaViews();

        m_statusLabel->setText(QStringLiteral("Yendo a '%1'...").arg(preset.name));
        QPointer<PtzDockWidget> self(this);
        m_client.absoluteMove(
            cam.ptzServiceUrl, cam.profileToken, cam.username, cam.password,
            cam.clockOffsetSecs, preset.pan, preset.tilt, preset.zoom, true, 0.85,
            [self, cam, preset](bool ok, const QString &err) {
                if (!self) {
                    return;
                }
                if (ok) {
                    self->m_statusLabel->setText(
                        QStringLiteral("Preset '%1' aplicado.").arg(preset.name));
                    self->scheduleStatusRefresh();
                    return;
                }
                if (!preset.cameraPresetToken.isEmpty()) {
                    self->m_client.gotoPreset(
                        cam.ptzServiceUrl, cam.profileToken, preset.cameraPresetToken,
                        cam.username, cam.password, cam.clockOffsetSecs,
                        [self, preset](bool ok2, const QString &err2) {
                            if (!self) {
                                return;
                            }
                            self->m_statusLabel->setText(
                                ok2 ? QStringLiteral("Preset '%1' aplicado.").arg(preset.name)
                                    : QStringLiteral("Error al ir a '%1': %2")
                                          .arg(preset.name, err2));
                            if (ok2) {
                                self->scheduleStatusRefresh();
                            }
                        });
                    return;
                }
                self->m_statusLabel->setText(
                    QStringLiteral("Error al mover a '%1': %2").arg(preset.name, err));
            });
        return;
    }

    if (!preset.cameraPresetToken.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("Yendo a '%1'...").arg(preset.name));
        QPointer<PtzDockWidget> self(this);
        m_client.gotoPreset(
            cam.ptzServiceUrl, cam.profileToken, preset.cameraPresetToken,
            cam.username, cam.password, cam.clockOffsetSecs,
            [self, preset](bool ok, const QString &err) {
                if (!self) {
                    return;
                }
                self->m_statusLabel->setText(
                    ok ? QStringLiteral("Preset '%1' aplicado.").arg(preset.name)
                       : QStringLiteral("Error al ir a '%1': %2").arg(preset.name, err));
                if (ok) {
                    self->scheduleStatusRefresh();
                }
            });
    }
}

void PtzDockWidget::onUpdateVisualPresetPhotoClicked()
{
    PtzCameraEntry cam;
    if (!currentCamera(cam) || !m_visualPresetsList) {
        return;
    }
    QListWidgetItem *item = m_visualPresetsList->currentItem();
    if (!item) {
        return;
    }
    const QString id = item->data(Qt::UserRole).toString();
    m_statusLabel->setText(QStringLiteral("Actualizando miniatura..."));

    QPointer<PtzDockWidget> self(this);
    captureCurrentFrameForCamera(
        cam,
        [self, cam, id](const QImage &img) {
            if (!self || img.isNull()) {
                if (self) {
                    self->m_statusLabel->setText(QStringLiteral("No se pudo capturar miniatura."));
                }
                return;
            }
            for (SavedPtzPreset &p : self->m_activeProfile.savedPresets) {
                if (p.id == id) {
                    p.thumbnailFile = self->m_store.savePresetThumbnail(
                        self->m_activeProfile.cameraKey, p.id, img);
                    self->m_store.saveProfile(self->m_activeProfile);
                    self->refreshVisualPresetsList();
                    self->m_statusLabel->setText(QStringLiteral("Miniatura actualizada."));
                    return;
                }
            }
        });
}

void PtzDockWidget::onRenameVisualPresetClicked()
{
    if (!m_visualPresetsList) {
        return;
    }
    QListWidgetItem *item = m_visualPresetsList->currentItem();
    if (!item) {
        return;
    }
    const QString id = item->data(Qt::UserRole).toString();
    for (SavedPtzPreset &p : m_activeProfile.savedPresets) {
        if (p.id == id) {
            bool ok = false;
            const QString nextName = QInputDialog::getText(
                this, QStringLiteral("Renombrar vista"),
                QStringLiteral("Nuevo nombre:"), QLineEdit::Normal, p.name, &ok);
            if (ok && !nextName.trimmed().isEmpty()) {
                p.name = nextName.trimmed();
                m_store.saveProfile(m_activeProfile);
                refreshVisualPresetsList();
            }
            return;
        }
    }
}

void PtzDockWidget::onDeleteVisualPresetClicked()
{
    if (!m_visualPresetsList) {
        return;
    }
    QListWidgetItem *item = m_visualPresetsList->currentItem();
    if (!item) {
        return;
    }
    const QString id = item->data(Qt::UserRole).toString();
    for (int i = 0; i < static_cast<int>(m_activeProfile.savedPresets.size()); ++i) {
        if (m_activeProfile.savedPresets.at(i).id == id) {
            m_store.removePresetThumbnail(
                m_activeProfile.cameraKey, m_activeProfile.savedPresets.at(i).thumbnailFile);
            m_activeProfile.savedPresets.removeAt(i);
            m_store.saveProfile(m_activeProfile);
            refreshVisualPresetsList();
            m_statusLabel->setText(QStringLiteral("Vista eliminada."));
            return;
        }
    }
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
                self->m_statusLabel->setText(QStringLiteral("Presets ONVIF: %1").arg(err));
                return;
            }
            for (const PtzPreset &p : presets) {
                auto *item = new QListWidgetItem(p.name, self->m_presetsList);
                item->setData(Qt::UserRole, p.token);
            }
            self->m_statusLabel->setText(
                QStringLiteral("%1 preset(s) ONVIF cargado(s).").arg(presets.size()));
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
            self->m_statusLabel->setText(
                ok ? QStringLiteral("Preset aplicado.")
                   : QStringLiteral("Error al ir al preset: %1").arg(err));
            if (ok) {
                self->scheduleStatusRefresh();
            }
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
        this, QStringLiteral("Guardar preset ONVIF"),
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
                self->m_statusLabel->setText(
                    QStringLiteral("Error guardando preset: %1").arg(err));
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
                self->m_statusLabel->setText(
                    QStringLiteral("Error borrando preset: %1").arg(err));
            }
        });
}

} // namespace netdev
