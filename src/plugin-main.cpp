/*
TitleSync
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
#include "i18n.hpp"
#include "settings-dialog.hpp"
#include "switcher.hpp"
#include "voice.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>

#include <filesystem>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

kgs::Switcher *g_switcher = nullptr;
QPointer<QWidget> g_dock;

// Tastenkürzel (Einstellungen → Tastenkürzel); die Belegung speichert OBS mit der Szenensammlung
struct Hotkey {
	const char *name;
	const char *description;
	void (*action)();
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};
Hotkey g_hotkeys[] = {
	{"kgs_check_now", "Main.TitleSyncCheckNow",
	 [] {
		 g_switcher->checkNow();
	 }},
	{"kgs_test", "Main.TitleSyncTestChangeNothing",
	 [] {
		 g_switcher->testNow();
	 }},
	{"kgs_toggle", "Main.TitleSyncPauseResume",
	 [] {
		 g_switcher->setEnabled(!g_switcher->settings().enabled);
	 }},
};

void onHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && g_switcher)
		static_cast<Hotkey *>(data)->action();
}

void registerHotkeys()
{
	for (Hotkey &hotkey : g_hotkeys)
		hotkey.id = obs_hotkey_register_frontend(hotkey.name, kgs::Tr(hotkey.description), onHotkey, &hotkey);
}

void unregisterHotkeys()
{
	for (Hotkey &hotkey : g_hotkeys) {
		if (hotkey.id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey.id);
		hotkey.id = OBS_INVALID_HOTKEY_ID;
	}
}

void onSave(obs_data_t *data, bool saving, void *)
{
	for (Hotkey &hotkey : g_hotkeys) {
		if (saving) {
			obs_data_array_t *bindings = obs_hotkey_save(hotkey.id);
			obs_data_set_array(data, hotkey.name, bindings);
			obs_data_array_release(bindings);
		} else {
			obs_data_array_t *bindings = obs_data_get_array(data, hotkey.name);
			obs_hotkey_load(hotkey.id, bindings);
			obs_data_array_release(bindings);
		}
	}
}

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
		// Audioquelle loslassen, solange sie noch existiert
		kgs::voice::shutdown();
		break;
	default:
		break;
	}
}

// Bis Version 1.3 hieß das Plugin „KI Game Switcher“ – Einstellungen, Logins und Sprachmodell übernehmen
void migrateOldConfig()
{
	namespace fs = std::filesystem;
	std::string current = kgs::configPath("");
	while (!current.empty() && (current.back() == '/' || current.back() == '\\'))
		current.pop_back();
	const fs::path newDir = fs::u8path(current);
	const fs::path oldDir = newDir.parent_path() / "ki-game-switcher";
	std::error_code error;
	if (fs::exists(newDir / "settings.json", error) || !fs::exists(oldDir / "settings.json", error))
		return;

	fs::create_directories(newDir, error);
	for (const char *file : {"settings.json", "twitch.json", "youtube.json"}) {
		if (fs::exists(oldDir / file, error))
			fs::copy_file(oldDir / file, newDir / file, fs::copy_options::skip_existing, error);
	}
	if (fs::exists(oldDir / "models", error))
		fs::copy(oldDir / "models", newDir / "models",
			 fs::copy_options::recursive | fs::copy_options::skip_existing, error);

	// Das Dock hat eine neue Kennung – OBS blendet es zunächst aus, also einmal zeigen
	kgs::Settings settings = kgs::loadSettings();
	settings.dockIntroduced = false;
	kgs::saveSettings(settings);
	obs_log(LOG_INFO, "Einstellungen von „KI Game Switcher“ übernommen");
}

void openSettings(void *)
{
	kgs::SettingsDialog::open(g_switcher, static_cast<QWidget *>(obs_frontend_get_main_window()));
}

} // namespace

MODULE_EXPORT const char *obs_module_name(void)
{
	return "TitleSync";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}

bool obs_module_load(void)
{
	migrateOldConfig();
	kgs::i18nInit(kgs::loadSettings().uiLanguage);
	kgs::httpGlobalInit();
	kgs::captureInfoStart();
	g_switcher = new kgs::Switcher();

	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (mainWindow) {
		auto *dock = new kgs::SwitcherDock(g_switcher, mainWindow);
		if (obs_frontend_add_dock_by_id("titlesync", "TitleSync", dock))
			g_dock = dock;
		else
			delete dock;
		obs_frontend_add_tools_menu_item(kgs::Tr("Main.TitleSyncSettings"), openSettings, nullptr);
	}
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	registerHotkeys();
	obs_frontend_add_save_callback(onSave, nullptr);

	obs_log(LOG_INFO, "geladen (Version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_frontend_remove_save_callback(onSave, nullptr);
	unregisterHotkeys();
	if (g_switcher) {
		g_switcher->stop();
		delete g_switcher;
		g_switcher = nullptr;
	}
	kgs::voice::shutdown();
	kgs::captureInfoStop();
	kgs::httpGlobalCleanup();
	kgs::i18nFree();
	obs_log(LOG_INFO, "entladen");
}
