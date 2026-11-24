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
#include <QDialog>
#include <QObject>
#include <QString>
#include <QWidget>

#include <cstring>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-network-device", "en-US")

namespace {

constexpr const char *kPtzDockId = "obs_network_device_ptz_dock";

bool g_frontendExited = false;

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
        obs_frontend_remove_dock(kPtzDockId);
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

    obs_source_t *sceneSource = obs_frontend_get_current_scene();
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
    auto *ptzDock = new netdev::PtzDockWidget(mainWin);
    obs_frontend_add_dock_by_id(kPtzDockId, obs_module_text("NetworkDevice.PtzDockTitle"), ptzDock);

    auto *toolsAction = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction(obs_module_text("NetworkDevice.ToolsMenu")));
    if (toolsAction) {
        QObject::connect(toolsAction, &QAction::triggered, toolsAction, []() {
            openExplorerAndCreateSource();
        });
    }

    obs_frontend_add_event_callback(onFrontendEvent, nullptr);

    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    if (!g_frontendExited) {
        obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
        obs_frontend_remove_dock(kPtzDockId);
    }
    obs_log(LOG_INFO, "plugin unloaded");
}
