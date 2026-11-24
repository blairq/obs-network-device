/*
obs-network-device
Copyright (C) 2026 Jorge (blairq)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "core/device-registry.hpp"
#include "plugin-support.h"
#include "source/network-device-source.hpp"
#include "ui/device-explorer-dialog.hpp"
#include "ui/ptz-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QList>
#include <QMenu>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <cstring>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-network-device", "en-US")

namespace {

constexpr const char *kPtzDockId = "obs_network_device_ptz_dock";

bool g_frontendExited = false;
QPointer<netdev::PtzDockWidget> g_ptzDock;
QObject *g_contextMenuFilter = nullptr;

obs_source_t *getEditableSceneSource()
{
    obs_source_t *sceneSource = nullptr;
    if (obs_frontend_preview_program_mode_active()) {
        sceneSource = obs_frontend_get_current_preview_scene();
    }
    if (!sceneSource) {
        sceneSource = obs_frontend_get_current_scene();
    }
    return sceneSource;
}

struct SelectedPtzScanContext {
    QString foundUuid;
};

bool findSelectedPtzSourceInScene(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
    Q_UNUSED(scene);
    auto *ctx = static_cast<SelectedPtzScanContext *>(param);
    if (!ctx || !item) {
        return true;
    }

    if (obs_sceneitem_is_group(item)) {
        obs_sceneitem_group_enum_items(item, findSelectedPtzSourceInScene, ctx);
        if (!ctx->foundUuid.isEmpty()) {
            return false;
        }
    }

    if (!obs_sceneitem_selected(item)) {
        return true;
    }

    obs_source_t *src = obs_sceneitem_get_source(item);
    if (!src) {
        return true;
    }

    const char *id = obs_source_get_id(src);
    if (id && std::strcmp(id, "network_device_source") == 0) {
        const char *uuid = obs_source_get_uuid(src);
        if (uuid && uuid[0] != '\0') {
            ctx->foundUuid = QString::fromUtf8(uuid);
            return false;
        }
    }
    return true;
}

QString findSelectedNetworkDeviceUuid()
{
    obs_source_t *sceneSource = getEditableSceneSource();
    if (!sceneSource) {
        return {};
    }

    SelectedPtzScanContext ctx;
    if (obs_scene_t *scene = obs_scene_from_source(sceneSource)) {
        obs_scene_enum_items(scene, findSelectedPtzSourceInScene, &ctx);
    }
    obs_source_release(sceneSource);
    return ctx.foundUuid;
}

void cleanupContextMenuFilter()
{
    if (g_contextMenuFilter) {
        if (qApp) {
            qApp->removeEventFilter(g_contextMenuFilter);
        }
        delete g_contextMenuFilter;
        g_contextMenuFilter = nullptr;
    }
}

class SourceContextMenuFilter final : public QObject {
public:
    explicit SourceContextMenuFilter(QWidget *mainWin, QObject *parent = nullptr)
        : QObject(parent), m_mainWin(mainWin)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (g_frontendExited || !m_mainWin) {
            return false;
        }

        const QEvent::Type evType = event->type();
        if (evType != QEvent::Polish && evType != QEvent::Show) {
            return false;
        }

        auto *menu = qobject_cast<QMenu *>(watched);
        if (!menu || menu->parentWidget() != m_mainWin || !menu->objectName().isEmpty()) {
            return false;
        }

        if (menu->property("obs_netdev_ptz_injected").toBool()) {
            return false;
        }

        const QList<QAction *> actions = menu->actions();
        bool hasCopySource = false;
        bool hasRemoveSource = false;
        for (QAction *act : actions) {
            if (!act) {
                continue;
            }
            const QString objName = act->objectName();
            if (objName == QLatin1String("actionCopySource")) {
                hasCopySource = true;
            } else if (objName == QLatin1String("actionRemoveSource")) {
                hasRemoveSource = true;
            }
        }
        if (!hasCopySource || !hasRemoveSource) {
            return false;
        }

        const QString selectedUuid = findSelectedNetworkDeviceUuid();
        if (selectedUuid.isEmpty()) {
            return false;
        }

        menu->setProperty("obs_netdev_ptz_injected", true);
        QAction *beforeAction =
            (actions.size() >= 2) ? actions.at(actions.size() - 2) : nullptr;
        auto *ptzAction = new QAction(
            QString::fromUtf8(obs_module_text("NetworkDevice.ContextMenuOpenPtz")), menu);
        QObject::connect(ptzAction, &QAction::triggered, ptzAction, [selectedUuid]() {
            QTimer::singleShot(0, qApp, [selectedUuid]() {
                showNetworkDevicePtzDock(selectedUuid);
            });
        });
        menu->insertAction(beforeAction, ptzAction);

        if (evType == QEvent::Show) {
            menu->adjustSize();
        }
        return false;
    }

private:
    QPointer<QWidget> m_mainWin;
};

struct SceneScanContext {
    QString foundName;
};

bool findFirstPtzSourceInScene(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
    Q_UNUSED(scene);
    auto *ctx = static_cast<SceneScanContext *>(param);
    obs_source_t *src = obs_sceneitem_get_source(item);
    if (!src) {
        return true;
    }

    const char *id = obs_source_get_id(src);
    if (id && std::strcmp(id, "network_device_source") == 0) {
        const char *uuid = obs_source_get_uuid(src);
        if (uuid && uuid[0] != '\0') {
            netdev::PtzCameraEntry entry;
            const QString qUuid = QString::fromUtf8(uuid);
            if (netdev::DeviceRegistry::instance().cameraById(qUuid, entry)) {
                ctx->foundName = qUuid;
                return false;
            }
        }
    }
    return true;
}

void notifyActivePtzForScene(obs_source_t *sceneSource)
{
    if (!sceneSource) {
        return;
    }
    obs_scene_t *scene = obs_scene_from_source(sceneSource);
    if (!scene) {
        return;
    }
    SceneScanContext ctx;
    obs_scene_enum_items(scene, findFirstPtzSourceInScene, &ctx);
    if (!ctx.foundName.isEmpty()) {
        netdev::DeviceRegistry::instance().notifyActiveSourceChanged(ctx.foundName);
    }
}

void onFrontendEvent(enum obs_frontend_event event, void *privateData)
{
    Q_UNUSED(privateData);
    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
        obs_source_t *cur = obs_frontend_get_current_scene();
        notifyActivePtzForScene(cur);
        obs_source_release(cur);
        break;
    }
    case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED: {
        obs_source_t *prev = obs_frontend_get_current_preview_scene();
        notifyActivePtzForScene(prev);
        obs_source_release(prev);
        break;
    }
    case OBS_FRONTEND_EVENT_EXIT: {
        g_frontendExited = true;
        cleanupContextMenuFilter();
        obs_frontend_remove_dock(kPtzDockId);
        g_ptzDock.clear();
        break;
    }
    default:
        break;
    }
}

QString makeUniqueSourceName(const QString &baseName)
{
    const QString cleanBase = baseName.trimmed().isEmpty()
                                  ? QStringLiteral("Camara ONVIF")
                                  : baseName.trimmed();
    QString candidate = cleanBase;
    int suffix = 2;
    while (obs_source_t *existing = obs_get_source_by_name(candidate.toUtf8().constData())) {
        obs_source_release(existing);
        candidate = QStringLiteral("%1 (%2)").arg(cleanBase).arg(suffix++);
    }
    return candidate;
}

void openExplorerAndCreateSource()
{
    auto *mainWin = static_cast<QWidget *>(obs_frontend_get_main_window());
    netdev::DeviceExplorerDialog dlg(netdev::SourceConfig{}, mainWin);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const netdev::SourceConfig cfg = dlg.selectedConfig();
    if (cfg.rtspUri.isEmpty()) {
        return;
    }

    obs_source_t *sceneSource = getEditableSceneSource();
    if (!sceneSource) {
        return;
    }

    obs_scene_t *scene = obs_scene_from_source(sceneSource);
    if (scene) {
        const QString sourceName = makeUniqueSourceName(cfg.deviceName);
        obs_data_t *settings = obs_data_create();
        applySourceConfigToData(settings, cfg);

        obs_source_t *newSource = obs_source_create(
            "network_device_source", sourceName.toUtf8().constData(), settings, nullptr);
        if (newSource) {
            obs_scene_add(scene, newSource);
            obs_source_release(newSource);
        }
        obs_data_release(settings);
    }

    obs_source_release(sceneSource);
}

} // namespace

void showNetworkDevicePtzDock(const QString &sourceUuid)
{
    if (g_frontendExited || !qApp) {
        return;
    }

    if (QThread::currentThread() != qApp->thread()) {
        QMetaObject::invokeMethod(
            qApp, [sourceUuid]() { showNetworkDevicePtzDock(sourceUuid); }, Qt::QueuedConnection);
        return;
    }

    if (!g_ptzDock) {
        return;
    }

    if (!sourceUuid.isEmpty()) {
        g_ptzDock->selectCameraBySourceUuid(sourceUuid);
    }

    QDockWidget *dock = nullptr;
    for (QWidget *w = g_ptzDock->parentWidget(); w; w = w->parentWidget()) {
        if (auto *candidate = qobject_cast<QDockWidget *>(w)) {
            dock = candidate;
            break;
        }
    }
    if (!dock) {
        if (auto *mainWin = static_cast<QWidget *>(obs_frontend_get_main_window())) {
            dock = mainWin->findChild<QDockWidget *>(QString::fromLatin1(kPtzDockId));
        }
    }

    if (dock) {
        if (dock->isHidden()) {
            dock->show();
        }
        dock->raise();
        dock->activateWindow();
    }
}

const char *obs_module_name(void)
{
    return "OBS Network Device";
}

const char *obs_module_description(void)
{
    return "Auto-discovery network camera source (ONVIF) with integrated PTZ control.";
}

bool obs_module_load(void)
{
    registerNetworkDeviceSource();

    auto *mainWin = static_cast<QWidget *>(obs_frontend_get_main_window());
    QString storageDir;
    if (char *cfgPath = obs_module_config_path("cameras")) {
        storageDir = QString::fromUtf8(cfgPath);
        bfree(cfgPath);
        QDir().mkpath(storageDir);
    }
    auto *ptzDock = new netdev::PtzDockWidget(storageDir, mainWin);
    g_ptzDock = ptzDock;
    obs_frontend_add_dock_by_id(kPtzDockId, obs_module_text("NetworkDevice.PtzDockTitle"), ptzDock);

    if (qApp) {
        cleanupContextMenuFilter();
        g_contextMenuFilter = new SourceContextMenuFilter(mainWin);
        qApp->installEventFilter(g_contextMenuFilter);
    }

    auto *toolsAction = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction(obs_module_text("NetworkDevice.ToolsMenu")));
    if (toolsAction) {
        QObject::connect(toolsAction, &QAction::triggered, toolsAction, []() {
            openExplorerAndCreateSource();
        });
    }

    auto *ptzToolsAction = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction(obs_module_text("NetworkDevice.ToolsMenuPtz")));
    if (ptzToolsAction) {
        QObject::connect(ptzToolsAction, &QAction::triggered, ptzToolsAction, []() {
            showNetworkDevicePtzDock();
        });
    }

    obs_frontend_add_event_callback(onFrontendEvent, nullptr);

    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    cleanupContextMenuFilter();
    if (!g_frontendExited) {
        obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
        obs_frontend_remove_dock(kPtzDockId);
    }
    g_ptzDock.clear();
    obs_log(LOG_INFO, "plugin unloaded");
}
