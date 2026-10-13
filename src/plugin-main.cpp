/*
KI Game Switcher
Copyright (C) 2026 Belloccc

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "capture-info.hpp"
#include "dock.hpp"
#include "http.hpp"
#include "settings-dialog.hpp"
#include "switcher.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

kgs::Switcher *g_switcher = nullptr;
QPointer<QWidget> g_dock;

// OBS blendet neue Plugin-Docks standardmäßig aus. Beim allerersten Start zeigen wir es einmal,
// danach merkt sich OBS selbst, ob es offen ist.
void introduceDock()
{
	kgs::Settings settings = g_switcher->settings();
	if (settings.dockIntroduced || !g_dock)
		return;
	if (auto *dockWidget = qobject_cast<QDockWidget *>(g_dock->parentWidget())) {
		dockWidget->resize(360, 720);
		dockWidget->show();
	}
	settings.dockIntroduced = true;
	g_switcher->setSettings(settings);
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		g_switcher->setStreaming(obs_frontend_streaming_active());
		g_switcher->start();
		introduceDock();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		g_switcher->setStreaming(true);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		g_switcher->setStreaming(false);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// Vor dem Abbau der Grafik anhalten, damit der Worker nicht mehr rendert
		g_switcher->stop();
		break;
	default:
		break;
	}
}

void openSettings(void *)
{
	kgs::SettingsDialog::open(g_switcher, static_cast<QWidget *>(obs_frontend_get_main_window()));
}

} // namespace

MODULE_EXPORT const char *obs_module_name(void)
{
	return "KI Game Switcher";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Erkennt per KI, welches Spiel im Stream läuft, und stellt Twitch-Titel und -Kategorie automatisch um.";
}

bool obs_module_load(void)
{
	kgs::httpGlobalInit();
	kgs::captureInfoStart();
	g_switcher = new kgs::Switcher();

	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (mainWindow) {
		auto *dock = new kgs::SwitcherDock(g_switcher, mainWindow);
		if (obs_frontend_add_dock_by_id("ki-game-switcher", "KI Game Switcher", dock))
			g_dock = dock;
		else
			delete dock;
		obs_frontend_add_tools_menu_item("KI Game Switcher – Einstellungen", openSettings, nullptr);
	}
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	obs_log(LOG_INFO, "geladen (Version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	if (g_switcher) {
		g_switcher->stop();
		delete g_switcher;
		g_switcher = nullptr;
	}
	kgs::captureInfoStop();
	kgs::httpGlobalCleanup();
	obs_log(LOG_INFO, "entladen");
}
