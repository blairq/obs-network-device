#pragma once

#include "core/device-provider.hpp"
#include "core/device-types.hpp"

#include <QDialog>
#include <QImage>
#include <QMap>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace netdev {

class OnvifProvider;

class DeviceExplorerDialog : public QDialog {
    Q_OBJECT

public:
    explicit DeviceExplorerDialog(const SourceConfig &initialConfig = SourceConfig{},
                                  QWidget *parent = nullptr);
    ~DeviceExplorerDialog() override = default;

    SourceConfig selectedConfig() const;

private slots:
    void onStartScan();
    void onScanFinished();
    void onDeviceFound(const netdev::DiscoveredDevice &device);
    void onManualAdd();
    void onDeviceSelected();
    void onInspectClicked();
    void onDeviceInspected(const netdev::DiscoveredDevice &device);
    void onAuthRequired(const QString &serviceUrl, const QString &message);
    void onInspectionFailed(const QString &serviceUrl, const QString &error);
    void onRefreshSnapshot();
    void onSnapshotReady(const QImage &image);
    void onSnapshotError(const QString &error);
    void onProfileRowChanged();
    void onToggleShowPassword(bool checked);

private:
    void setupUi(const SourceConfig &initialConfig);
    void upsertDeviceInTree(const DiscoveredDevice &device);
    void populateDeviceDetails(const DiscoveredDevice &device);
    void updateSnapshotDisplay(const QImage &image);
    void updateAcceptButtonState();
    QString currentDeviceId() const;

    OnvifProvider *m_provider = nullptr;
    QMap<QString, DiscoveredDevice> m_devices;
    QString m_activeDeviceId;
    QImage m_lastSnapshot;
    QString m_lastSnapshotUri;

    // Left panel
    QPushButton *m_scanBtn = nullptr;
    QComboBox *m_protocolFilter = nullptr;
    QLabel *m_scanStatusLabel = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    QLineEdit *m_manualHostEdit = nullptr;
    QSpinBox *m_manualPortSpin = nullptr;
    QPushButton *m_manualAddBtn = nullptr;
    QTreeWidget *m_deviceTree = nullptr;

    // Right panel - Info & Auth
    QLabel *m_deviceTitleLabel = nullptr;
    QLabel *m_deviceMetaLabel = nullptr;
    QLabel *m_serviceUrlLabel = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passEdit = nullptr;
    QPushButton *m_showPassBtn = nullptr;
    QPushButton *m_inspectBtn = nullptr;
    QLabel *m_authBannerLabel = nullptr;

    // Right panel - Snapshot
    QLabel *m_snapshotLabel = nullptr;
    QPushButton *m_refreshSnapBtn = nullptr;
    QLabel *m_snapshotStatusLabel = nullptr;

    // Right panel - Profiles & Options
    QTableWidget *m_profilesTable = nullptr;
    QComboBox *m_transportCombo = nullptr;
    QCheckBox *m_lowLatencyCheck = nullptr;
    QCheckBox *m_hwDecodeCheck = nullptr;
    QCheckBox *m_ptzCheck = nullptr;

    // Bottom buttons
    QPushButton *m_acceptBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
};

} // namespace netdev
