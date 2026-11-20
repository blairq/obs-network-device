#include "device-explorer-dialog.hpp"

#include "onvif/onvif-client.hpp"
#include "onvif/onvif-provider.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSize>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

namespace netdev {

DeviceExplorerDialog::DeviceExplorerDialog(const SourceConfig &initialConfig, QWidget *parent)
    : QDialog(parent), m_provider(new OnvifProvider(this))
{
    setWindowTitle(QStringLiteral("Explorador de dispositivos de red (ONVIF)"));
    resize(960, 650);
    setMinimumSize(820, 560);

    setupUi(initialConfig);

    connect(m_provider, &IDeviceProvider::deviceFound, this, &DeviceExplorerDialog::onDeviceFound);
    connect(m_provider, &IDeviceProvider::scanFinished, this, &DeviceExplorerDialog::onScanFinished);
    connect(m_provider, &IDeviceProvider::deviceInspected, this, &DeviceExplorerDialog::onDeviceInspected);
    connect(m_provider, &IDeviceProvider::authRequired, this, &DeviceExplorerDialog::onAuthRequired);
    connect(m_provider, &IDeviceProvider::inspectionFailed, this, &DeviceExplorerDialog::onInspectionFailed);
    connect(m_provider, &IDeviceProvider::snapshotReady, this, &DeviceExplorerDialog::onSnapshotReady);
    connect(m_provider, &IDeviceProvider::snapshotError, this, &DeviceExplorerDialog::onSnapshotError);

    if (!initialConfig.deviceServiceUrl.isEmpty()) {
        DiscoveredDevice seeded;
        seeded.protocol = initialConfig.protocol;
        seeded.deviceServiceUrl = initialConfig.deviceServiceUrl;
        seeded.id = initialConfig.deviceServiceUrl;
        seeded.mediaServiceUrl = initialConfig.mediaServiceUrl;
        seeded.ptzServiceUrl = initialConfig.ptzServiceUrl;
        seeded.name = initialConfig.deviceName.isEmpty() ? QStringLiteral("Camara configurada")
                                                         : initialConfig.deviceName;
        seeded.hasPtz = initialConfig.hasPtz;
        const QUrl u(initialConfig.deviceServiceUrl);
        seeded.ip = u.host();
        seeded.port = static_cast<quint16>(u.port(80));
        m_devices.insert(seeded.id, seeded);
        upsertDeviceInTree(seeded);
    }

    QTimer::singleShot(50, this, &DeviceExplorerDialog::onStartScan);
}

void DeviceExplorerDialog::setupUi(const SourceConfig &initialConfig)
{
    auto *rootLayout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // --- Left Panel ---
    auto *leftWidget = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 4, 0);

    auto *scanGroup = new QGroupBox(QStringLiteral("Dispositivos en la red"), leftWidget);
    auto *scanLayout = new QVBoxLayout(scanGroup);

    auto *topScanRow = new QHBoxLayout();
    m_scanBtn = new QPushButton(QStringLiteral("Escanear red"), scanGroup);
    m_protocolFilter = new QComboBox(scanGroup);
    m_protocolFilter->addItem(QStringLiteral("Todos los protocolos"), QStringLiteral("all"));
    m_protocolFilter->addItem(QStringLiteral("ONVIF"), QStringLiteral("onvif"));
    topScanRow->addWidget(m_scanBtn, 1);
    topScanRow->addWidget(m_protocolFilter);
    scanLayout->addLayout(topScanRow);

    m_scanStatusLabel = new QLabel(QStringLiteral("Listo para escanear la red local."), scanGroup);
    scanLayout->addWidget(m_scanStatusLabel);

    m_scanProgress = new QProgressBar(scanGroup);
    m_scanProgress->setRange(0, 0);
    m_scanProgress->setMaximumHeight(6);
    m_scanProgress->setTextVisible(false);
    m_scanProgress->setVisible(false);
    scanLayout->addWidget(m_scanProgress);

    auto *manualRow = new QHBoxLayout();
    m_manualHostEdit = new QLineEdit(scanGroup);
    m_manualHostEdit->setPlaceholderText(QStringLiteral("IP manual (ej. 192.168.1.50)"));
    m_manualPortSpin = new QSpinBox(scanGroup);
    m_manualPortSpin->setRange(1, 65535);
    m_manualPortSpin->setValue(80);
    m_manualPortSpin->setPrefix(QStringLiteral(":"));
    m_manualAddBtn = new QPushButton(QStringLiteral("Agregar"), scanGroup);
    manualRow->addWidget(m_manualHostEdit, 1);
    manualRow->addWidget(m_manualPortSpin);
    manualRow->addWidget(m_manualAddBtn);
    scanLayout->addLayout(manualRow);

    m_deviceTree = new QTreeWidget(scanGroup);
    m_deviceTree->setColumnCount(4);
    m_deviceTree->setHeaderLabels(
        {QStringLiteral("Dispositivo"), QStringLiteral("IP"), QStringLiteral("Tipo"), QStringLiteral("Estado")});
    m_deviceTree->setRootIsDecorated(false);
    m_deviceTree->setAlternatingRowColors(true);
    m_deviceTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_deviceTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_deviceTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_deviceTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    scanLayout->addWidget(m_deviceTree, 1);

    leftLayout->addWidget(scanGroup);

    // --- Right Panel ---
    auto *rightWidget = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(4, 0, 0, 0);

    auto *infoGroup = new QGroupBox(QStringLiteral("Dispositivo seleccionado"), rightWidget);
    auto *infoLayout = new QVBoxLayout(infoGroup);
    m_deviceTitleLabel = new QLabel(QStringLiteral("Ningun dispositivo seleccionado"), infoGroup);
    QFont titleFont = m_deviceTitleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    m_deviceTitleLabel->setFont(titleFont);
    m_deviceMetaLabel = new QLabel(QStringLiteral("Selecciona una camara de la lista o agrega una IP manual."), infoGroup);
    m_deviceMetaLabel->setWordWrap(true);
    m_serviceUrlLabel = new QLabel(infoGroup);
    m_serviceUrlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLayout->addWidget(m_deviceTitleLabel);
    infoLayout->addWidget(m_deviceMetaLabel);
    infoLayout->addWidget(m_serviceUrlLabel);
    rightLayout->addWidget(infoGroup);

    auto *authGroup = new QGroupBox(QStringLiteral("Autenticacion y escaneo del dispositivo"), rightWidget);
    auto *authLayout = new QVBoxLayout(authGroup);
    auto *credRow = new QHBoxLayout();
    m_userEdit = new QLineEdit(authGroup);
    m_userEdit->setPlaceholderText(QStringLiteral("Usuario (ej. admin)"));
    m_userEdit->setText(initialConfig.username);
    m_passEdit = new QLineEdit(authGroup);
    m_passEdit->setPlaceholderText(QStringLiteral("Contraseña"));
    m_passEdit->setEchoMode(QLineEdit::Password);
    m_passEdit->setText(initialConfig.password);
    m_showPassBtn = new QPushButton(QStringLiteral("Ver"), authGroup);
    m_showPassBtn->setCheckable(true);
    m_inspectBtn = new QPushButton(QStringLiteral("Conectar / Escanear dispositivo"), authGroup);
    m_inspectBtn->setEnabled(false);

    credRow->addWidget(new QLabel(QStringLiteral("Usuario:"), authGroup));
    credRow->addWidget(m_userEdit, 1);
    credRow->addWidget(new QLabel(QStringLiteral("Clave:"), authGroup));
    credRow->addWidget(m_passEdit, 1);
    credRow->addWidget(m_showPassBtn);
    credRow->addWidget(m_inspectBtn);
    authLayout->addLayout(credRow);

    m_authBannerLabel = new QLabel(authGroup);
    m_authBannerLabel->setWordWrap(true);
    m_authBannerLabel->setVisible(false);
    authLayout->addWidget(m_authBannerLabel);
    rightLayout->addWidget(authGroup);

    auto *snapGroup = new QGroupBox(QStringLiteral("Vista previa (Screenshot)"), rightWidget);
    auto *snapLayout = new QVBoxLayout(snapGroup);
    m_snapshotLabel = new QLabel(QStringLiteral("Sin captura disponible"), snapGroup);
    m_snapshotLabel->setFixedHeight(180);
    m_snapshotLabel->setAlignment(Qt::AlignCenter);
    m_snapshotLabel->setFrameShape(QFrame::StyledPanel);
    snapLayout->addWidget(m_snapshotLabel);

    auto *snapBar = new QHBoxLayout();
    m_snapshotStatusLabel = new QLabel(QString(), snapGroup);
    m_refreshSnapBtn = new QPushButton(QStringLiteral("Actualizar captura"), snapGroup);
    m_refreshSnapBtn->setEnabled(false);
    snapBar->addWidget(m_snapshotStatusLabel, 1);
    snapBar->addWidget(m_refreshSnapBtn);
    snapLayout->addLayout(snapBar);
    rightLayout->addWidget(snapGroup);

    auto *profilesGroup = new QGroupBox(QStringLiteral("Fuentes / Perfiles de video disponibles"), rightWidget);
    auto *profilesLayout = new QVBoxLayout(profilesGroup);
    m_profilesTable = new QTableWidget(0, 6, profilesGroup);
    m_profilesTable->setHorizontalHeaderLabels({
        QStringLiteral("Perfil"),
        QStringLiteral("Resolucion"),
        QStringLiteral("Codec"),
        QStringLiteral("FPS"),
        QStringLiteral("Audio"),
        QStringLiteral("PTZ"),
    });
    m_profilesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_profilesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_profilesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_profilesTable->verticalHeader()->setVisible(false);
    m_profilesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int col = 1; col < 6; ++col) {
        m_profilesTable->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    }
    profilesLayout->addWidget(m_profilesTable);
    rightLayout->addWidget(profilesGroup, 1);

    auto *optsGroup = new QGroupBox(QStringLiteral("Opciones de transmision"), rightWidget);
    auto *optsLayout = new QHBoxLayout(optsGroup);
    m_transportCombo = new QComboBox(optsGroup);
    m_transportCombo->addItem(QStringLiteral("RTSP sobre TCP (Recomendado)"), QStringLiteral("tcp"));
    m_transportCombo->addItem(QStringLiteral("RTSP sobre UDP"), QStringLiteral("udp"));
    if (initialConfig.rtspTransport == QStringLiteral("udp")) {
        m_transportCombo->setCurrentIndex(1);
    }
    m_hwDecodeCheck = new QCheckBox(QStringLiteral("Decodificacion por hardware"), optsGroup);
    m_hwDecodeCheck->setChecked(initialConfig.hwDecode);
    m_ptzCheck = new QCheckBox(QStringLiteral("Habilitar control PTZ automatico"), optsGroup);
    m_ptzCheck->setChecked(true);

    optsLayout->addWidget(new QLabel(QStringLiteral("Transporte:"), optsGroup));
    optsLayout->addWidget(m_transportCombo);
    optsLayout->addWidget(m_hwDecodeCheck);
    optsLayout->addWidget(m_ptzCheck);
    optsLayout->addStretch();
    rightLayout->addWidget(optsGroup);

    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    rootLayout->addWidget(splitter, 1);

    // Bottom buttons
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    m_acceptBtn = new QPushButton(QStringLiteral("Usar esta camara"), this);
    m_acceptBtn->setDefault(true);
    m_acceptBtn->setEnabled(false);
    m_cancelBtn = new QPushButton(QStringLiteral("Cancelar"), this);
    btnRow->addWidget(m_acceptBtn);
    btnRow->addWidget(m_cancelBtn);
    rootLayout->addLayout(btnRow);

    connect(m_scanBtn, &QPushButton::clicked, this, &DeviceExplorerDialog::onStartScan);
    connect(m_protocolFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const QString filter = m_protocolFilter->currentData().toString();
        for (int i = 0; i < m_deviceTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_deviceTree->topLevelItem(i);
            const QString id = item->data(0, Qt::UserRole).toString();
            const bool show = (filter == QStringLiteral("all")) ||
                              (m_devices.contains(id) &&
                               protocolDisplayName(m_devices.value(id).protocol)
                                       .compare(filter, Qt::CaseInsensitive) == 0);
            item->setHidden(!show);
        }
    });
    connect(m_manualAddBtn, &QPushButton::clicked, this, &DeviceExplorerDialog::onManualAdd);
    connect(m_manualHostEdit, &QLineEdit::returnPressed, this, &DeviceExplorerDialog::onManualAdd);
    connect(m_deviceTree, &QTreeWidget::itemSelectionChanged, this, &DeviceExplorerDialog::onDeviceSelected);
    connect(m_inspectBtn, &QPushButton::clicked, this, &DeviceExplorerDialog::onInspectClicked);
    connect(m_passEdit, &QLineEdit::returnPressed, this, &DeviceExplorerDialog::onInspectClicked);
    connect(m_showPassBtn, &QPushButton::toggled, this, &DeviceExplorerDialog::onToggleShowPassword);
    connect(m_refreshSnapBtn, &QPushButton::clicked, this, &DeviceExplorerDialog::onRefreshSnapshot);
    connect(m_profilesTable, &QTableWidget::itemSelectionChanged, this, &DeviceExplorerDialog::onProfileRowChanged);
    connect(m_acceptBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void DeviceExplorerDialog::onStartScan()
{
    m_scanBtn->setEnabled(false);
    m_scanBtn->setText(QStringLiteral("Escaneando..."));
    m_scanProgress->setVisible(true);
    m_scanStatusLabel->setText(QStringLiteral("Buscando dispositivos ONVIF en todas las redes locales..."));
    m_provider->startScan(3000);
}

void DeviceExplorerDialog::onScanFinished()
{
    m_scanBtn->setEnabled(true);
    m_scanBtn->setText(QStringLiteral("Escanear red"));
    m_scanProgress->setVisible(false);
    m_scanStatusLabel->setText(
        QStringLiteral("Escaneo finalizado: %1 dispositivo(s) encontrado(s).").arg(m_devices.size()));
}

void DeviceExplorerDialog::onDeviceFound(const DiscoveredDevice &device)
{
    if (m_devices.contains(device.id)) {
        DiscoveredDevice existing = m_devices.value(device.id);
        if (!existing.inspected) {
            existing.name = device.name;
            existing.model = device.model;
            existing.hasPtz = existing.hasPtz || device.hasPtz;
            m_devices.insert(device.id, existing);
            upsertDeviceInTree(existing);
        }
        return;
    }

    m_devices.insert(device.id, device);
    upsertDeviceInTree(device);
    m_scanStatusLabel->setText(
        QStringLiteral("Escaneando... %1 dispositivo(s) encontrado(s).").arg(m_devices.size()));
}

void DeviceExplorerDialog::onManualAdd()
{
    const QString rawHost = m_manualHostEdit->text().trimmed();
    if (rawHost.isEmpty()) {
        return;
    }

    const quint16 port = static_cast<quint16>(m_manualPortSpin->value());
    const QString serviceUrl = OnvifClient::normalizeDeviceServiceUrl(rawHost, port);
    if (serviceUrl.isEmpty()) {
        m_authBannerLabel->setStyleSheet(QStringLiteral("color: #ef4444; font-weight: bold;"));
        m_authBannerLabel->setText(QStringLiteral("IP o URL invalida."));
        m_authBannerLabel->setVisible(true);
        return;
    }

    const QUrl u(serviceUrl);
    DiscoveredDevice dev;
    dev.protocol = DeviceProtocol::Onvif;
    dev.id = serviceUrl;
    dev.deviceServiceUrl = serviceUrl;
    dev.ip = u.host();
    dev.port = static_cast<quint16>(u.port(80));
    dev.name = QStringLiteral("ONVIF (%1)").arg(dev.ip);

    m_devices.insert(dev.id, dev);
    upsertDeviceInTree(dev);

    for (int i = 0; i < m_deviceTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_deviceTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == dev.id) {
            m_deviceTree->setCurrentItem(item);
            break;
        }
    }
    m_manualHostEdit->clear();
}

void DeviceExplorerDialog::upsertDeviceInTree(const DiscoveredDevice &device)
{
    QTreeWidgetItem *target = nullptr;
    for (int i = 0; i < m_deviceTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_deviceTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == device.id) {
            target = item;
            break;
        }
    }
    if (!target) {
        target = new QTreeWidgetItem(m_deviceTree);
        target->setData(0, Qt::UserRole, device.id);
    }

    target->setText(0, device.name);
    target->setText(1, QStringLiteral("%1:%2").arg(device.ip).arg(device.port));
    target->setText(2, protocolDisplayName(device.protocol));

    QStringList tags;
    if (device.requiresAuth) tags << QStringLiteral("Requiere clave");
    if (device.hasPtz) tags << QStringLiteral("PTZ");
    if (device.inspected) tags << QStringLiteral("Listo");
    target->setText(3, tags.isEmpty() ? QStringLiteral("Detectado") : tags.join(QStringLiteral(" | ")));
}

QString DeviceExplorerDialog::currentDeviceId() const
{
    const auto items = m_deviceTree->selectedItems();
    if (items.isEmpty()) {
        return QString();
    }
    return items.first()->data(0, Qt::UserRole).toString();
}

void DeviceExplorerDialog::onDeviceSelected()
{
    const QString id = currentDeviceId();
    if (id.isEmpty() || !m_devices.contains(id)) {
        m_activeDeviceId.clear();
        m_inspectBtn->setEnabled(false);
        m_refreshSnapBtn->setEnabled(false);
        updateAcceptButtonState();
        return;
    }

    m_activeDeviceId = id;
    m_inspectBtn->setEnabled(true);
    const DiscoveredDevice dev = m_devices.value(id);
    populateDeviceDetails(dev);

    if (!dev.inspected) {
        onInspectClicked();
    }
}

void DeviceExplorerDialog::onInspectClicked()
{
    if (m_activeDeviceId.isEmpty() || !m_devices.contains(m_activeDeviceId)) {
        return;
    }

    const DiscoveredDevice dev = m_devices.value(m_activeDeviceId);
    m_authBannerLabel->setStyleSheet(QString());
    m_authBannerLabel->setText(QStringLiteral("Conectando e inspeccionando dispositivo ONVIF..."));
    m_authBannerLabel->setVisible(true);
    m_inspectBtn->setEnabled(false);

    m_provider->inspectDevice(dev.deviceServiceUrl, m_userEdit->text().trimmed(), m_passEdit->text());
}

void DeviceExplorerDialog::onDeviceInspected(const DiscoveredDevice &device)
{
    m_inspectBtn->setEnabled(true);

    // Match existing entry in m_devices by deviceServiceUrl
    QString key = device.id;
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
        if (it.value().deviceServiceUrl == device.deviceServiceUrl) {
            key = it.key();
            break;
        }
    }

    DiscoveredDevice merged = device;
    merged.id = key;
    merged.requiresAuth = false;
    m_devices.insert(key, merged);
    upsertDeviceInTree(merged);

    if (m_activeDeviceId == key || m_activeDeviceId.isEmpty()) {
        m_activeDeviceId = key;
        m_authBannerLabel->setStyleSheet(QStringLiteral("color: #22c55e; font-weight: bold;"));
        m_authBannerLabel->setText(
            QStringLiteral("Conectado correctamente (%1 perfil(es) encontrado(s)).")
                .arg(merged.profiles.size()));
        m_authBannerLabel->setVisible(true);
        populateDeviceDetails(merged);
    }
}

void DeviceExplorerDialog::onAuthRequired(const QString &serviceUrl, const QString &message)
{
    m_inspectBtn->setEnabled(true);
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (it.value().deviceServiceUrl == serviceUrl) {
            it.value().requiresAuth = true;
            upsertDeviceInTree(it.value());
        }
    }

    m_authBannerLabel->setStyleSheet(QStringLiteral("color: #f59e0b; font-weight: bold;"));
    m_authBannerLabel->setText(
        message + QStringLiteral(" Ingresa Usuario y Contraseña y toca 'Conectar / Escanear dispositivo'."));
    m_authBannerLabel->setVisible(true);
    m_userEdit->setFocus();
}

void DeviceExplorerDialog::onInspectionFailed(const QString &serviceUrl, const QString &error)
{
    Q_UNUSED(serviceUrl);
    m_inspectBtn->setEnabled(true);
    m_authBannerLabel->setStyleSheet(QStringLiteral("color: #ef4444; font-weight: bold;"));
    m_authBannerLabel->setText(QStringLiteral("Error al consultar el dispositivo: %1").arg(error));
    m_authBannerLabel->setVisible(true);
}

void DeviceExplorerDialog::populateDeviceDetails(const DiscoveredDevice &device)
{
    m_deviceTitleLabel->setText(device.name);
    QStringList meta;
    if (!device.manufacturer.isEmpty()) meta << QStringLiteral("Marca: %1").arg(device.manufacturer);
    if (!device.model.isEmpty()) meta << QStringLiteral("Modelo: %1").arg(device.model);
    if (!device.firmwareVersion.isEmpty()) meta << QStringLiteral("FW: %1").arg(device.firmwareVersion);
    if (!device.serialNumber.isEmpty()) meta << QStringLiteral("Serie: %1").arg(device.serialNumber);
    m_deviceMetaLabel->setText(meta.isEmpty() ? QStringLiteral("Haz clic en 'Conectar / Escanear dispositivo' para obtener detalles.")
                                              : meta.join(QStringLiteral("  |  ")));
    m_serviceUrlLabel->setText(QStringLiteral("Servicio ONVIF: %1").arg(device.deviceServiceUrl));

    m_ptzCheck->setEnabled(device.hasPtz);
    m_ptzCheck->setChecked(device.hasPtz);

    m_profilesTable->blockSignals(true);
    m_profilesTable->setRowCount(static_cast<int>(device.profiles.size()));

    for (int row = 0; row < static_cast<int>(device.profiles.size()); ++row) {
        const MediaProfile &p = device.profiles.at(row);
        const QString res = (p.width > 0 && p.height > 0)
                                ? QStringLiteral("%1x%2").arg(p.width).arg(p.height)
                                : QStringLiteral("-");
        const QString fpsStr = p.fps > 0.0 ? QString::number(p.fps, 'f', 0) : QStringLiteral("-");
        const QString codec = p.videoEncoding.isEmpty() ? QStringLiteral("-") : p.videoEncoding;

        auto *nameItem = new QTableWidgetItem(p.name);
        nameItem->setToolTip(p.rtspUri);
        m_profilesTable->setItem(row, 0, nameItem);
        m_profilesTable->setItem(row, 1, new QTableWidgetItem(res));
        m_profilesTable->setItem(row, 2, new QTableWidgetItem(codec));
        m_profilesTable->setItem(row, 3, new QTableWidgetItem(fpsStr));
        m_profilesTable->setItem(row, 4, new QTableWidgetItem(p.hasAudio ? QStringLiteral("Si") : QStringLiteral("No")));
        m_profilesTable->setItem(row, 5, new QTableWidgetItem(p.hasPtz ? QStringLiteral("Si") : QStringLiteral("No")));
    }

    m_profilesTable->blockSignals(false);

    if (!device.profiles.isEmpty()) {
        m_profilesTable->selectRow(0);
        onProfileRowChanged();
    } else {
        m_lastSnapshotUri.clear();
        m_refreshSnapBtn->setEnabled(false);
        m_snapshotLabel->setPixmap(QPixmap());
        m_snapshotLabel->setText(QStringLiteral("Sin captura disponible"));
        m_snapshotStatusLabel->clear();
        updateAcceptButtonState();
    }
}

void DeviceExplorerDialog::onProfileRowChanged()
{
    updateAcceptButtonState();

    if (m_activeDeviceId.isEmpty() || !m_devices.contains(m_activeDeviceId)) {
        return;
    }
    const DiscoveredDevice dev = m_devices.value(m_activeDeviceId);
    const int row = m_profilesTable->currentRow();
    if (row < 0 || row >= static_cast<int>(dev.profiles.size())) {
        return;
    }

    const MediaProfile &prof = dev.profiles.at(row);
    m_ptzCheck->setEnabled(prof.hasPtz || !dev.ptzServiceUrl.isEmpty());
    if (prof.hasPtz || !dev.ptzServiceUrl.isEmpty()) {
        m_ptzCheck->setChecked(true);
    }

    if (!prof.snapshotUri.isEmpty()) {
        const bool uriChanged = (prof.snapshotUri != m_lastSnapshotUri) || m_lastSnapshot.isNull();
        m_lastSnapshotUri = prof.snapshotUri;
        m_refreshSnapBtn->setEnabled(true);
        if (uriChanged) {
            onRefreshSnapshot();
        }
    } else {
        m_lastSnapshotUri.clear();
        m_refreshSnapBtn->setEnabled(false);
        m_snapshotStatusLabel->setText(QStringLiteral("Este perfil no reporto URL de captura."));
    }
}

void DeviceExplorerDialog::onRefreshSnapshot()
{
    if (m_lastSnapshotUri.isEmpty()) {
        return;
    }
    m_snapshotStatusLabel->setText(QStringLiteral("Descargando captura..."));
    m_provider->fetchSnapshot(m_lastSnapshotUri, m_userEdit->text().trimmed(), m_passEdit->text());
}

void DeviceExplorerDialog::onSnapshotReady(const QImage &image)
{
    m_lastSnapshot = image;
    updateSnapshotDisplay(image);
    m_snapshotStatusLabel->setText(
        QStringLiteral("Captura %1x%2 px").arg(image.width()).arg(image.height()));
}

void DeviceExplorerDialog::onSnapshotError(const QString &error)
{
    m_snapshotLabel->setPixmap(QPixmap());
    m_snapshotLabel->setText(QStringLiteral("Sin vista previa"));
    m_snapshotStatusLabel->setText(error);
}

void DeviceExplorerDialog::updateSnapshotDisplay(const QImage &image)
{
    if (image.isNull()) {
        return;
    }
    const QPixmap pix = QPixmap::fromImage(image);
    const QSize targetSize = m_snapshotLabel->size() - QSize(8, 8);
    m_snapshotLabel->setPixmap(
        pix.scaled(targetSize.expandedTo(QSize(240, 135)), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void DeviceExplorerDialog::onToggleShowPassword(bool checked)
{
    m_passEdit->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
}

void DeviceExplorerDialog::updateAcceptButtonState()
{
    if (m_activeDeviceId.isEmpty() || !m_devices.contains(m_activeDeviceId)) {
        m_acceptBtn->setEnabled(false);
        return;
    }
    const DiscoveredDevice dev = m_devices.value(m_activeDeviceId);
    const int row = m_profilesTable->currentRow();
    const bool valid = row >= 0 && row < static_cast<int>(dev.profiles.size()) &&
                       !dev.profiles.at(row).rtspUri.isEmpty();
    m_acceptBtn->setEnabled(valid);
}

SourceConfig DeviceExplorerDialog::selectedConfig() const
{
    SourceConfig cfg;
    if (m_activeDeviceId.isEmpty() || !m_devices.contains(m_activeDeviceId)) {
        return cfg;
    }

    const DiscoveredDevice dev = m_devices.value(m_activeDeviceId);
    const int row = m_profilesTable->currentRow();
    if (row < 0 || row >= static_cast<int>(dev.profiles.size())) {
        return cfg;
    }

    const MediaProfile &prof = dev.profiles.at(row);
    cfg.protocol = dev.protocol;
    cfg.deviceName = dev.name;
    cfg.deviceServiceUrl = dev.deviceServiceUrl;
    cfg.mediaServiceUrl = dev.mediaServiceUrl;
    cfg.ptzServiceUrl = dev.ptzServiceUrl;
    cfg.username = m_userEdit->text().trimmed();
    cfg.password = m_passEdit->text();
    cfg.profileToken = prof.token;
    cfg.profileName = prof.name;
    cfg.rtspUri = prof.rtspUri;
    cfg.snapshotUri = prof.snapshotUri;
    cfg.rtspTransport = m_transportCombo->currentData().toString();
    cfg.hwDecode = m_hwDecodeCheck->isChecked();
    cfg.hasPtz = m_ptzCheck->isChecked() && !dev.ptzServiceUrl.isEmpty();
    cfg.clockOffsetSecs = dev.clockOffsetSecs;
    return cfg;
}

} // namespace netdev
