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

#include "window-hint.hpp"

#include <obs-module.h>

#include <QString>
#include <QStringList>

#include <cstring>
#include <iterator>
#include <mutex>

namespace kgs {

namespace {

struct HookedCapture {
	obs_weak_source_t *weak;
	std::string executable;
	std::string title;
};

std::mutex g_mutex;
std::vector<HookedCapture> g_hooked;

std::vector<HookedCapture>::iterator findEntry(obs_source_t *source)
{
	for (auto it = g_hooked.begin(); it != g_hooked.end(); ++it) {
		if (obs_weak_source_references_source(it->weak, source))
			return it;
	}
	return g_hooked.end();
}

void onHooked(void *, calldata_t *data)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
	const char *executable = calldata_string(data, "executable");
	const char *title = calldata_string(data, "title");
	if (!source)
		return;

	std::lock_guard lock(g_mutex);
	auto it = findEntry(source);
	if (it == g_hooked.end()) {
		g_hooked.push_back({obs_source_get_weak_source(source), {}, {}});
		it = std::prev(g_hooked.end());
	}
	it->executable = executable ? executable : "";
	it->title = title ? title : "";
}

void onUnhooked(void *, calldata_t *data)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
	std::lock_guard lock(g_mutex);
	auto it = findEntry(source);
	if (it != g_hooked.end()) {
		obs_weak_source_release(it->weak);
		g_hooked.erase(it);
	}
}

void onSourceCreate(void *, calldata_t *data)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
	if (!source || std::strcmp(obs_source_get_unversioned_id(source), "game_capture") != 0)
		return;
	signal_handler_t *handler = obs_source_get_signal_handler(source);
	signal_handler_connect(handler, "hooked", onHooked, nullptr);
	signal_handler_connect(handler, "unhooked", onUnhooked, nullptr);
}

// Fensteraufnahmen speichern ihr Ziel als „Titel:Klasse:Programm“, Doppelpunkte als „#3A“ kodiert
CapturedApp parseWindowSetting(const char *value)
{
	const QStringList parts = QString::fromUtf8(value).split(':');
	auto decode = [](QString part) {
		return part.replace("#3A", ":").replace("#22", "#").toStdString();
	};
	if (parts.size() < 3)
		return {};
	return {decode(parts[2]), decode(parts[0]), false};
}

} // namespace

void captureInfoStart()
{
	signal_handler_connect(obs_get_signal_handler(), "source_create", onSourceCreate, nullptr);
}

void captureInfoStop()
{
	signal_handler_disconnect(obs_get_signal_handler(), "source_create", onSourceCreate, nullptr);
	std::lock_guard lock(g_mutex);
	for (HookedCapture &entry : g_hooked)
		obs_weak_source_release(entry.weak);
	g_hooked.clear();
}

std::vector<CapturedApp> activeCapturedApps()
{
	std::vector<CapturedApp> apps;
	{
		std::lock_guard lock(g_mutex);
		for (auto it = g_hooked.begin(); it != g_hooked.end();) {
			obs_source_t *source = obs_weak_source_get_source(it->weak);
			if (!source) {
				obs_weak_source_release(it->weak);
				it = g_hooked.erase(it);
				continue;
			}
			if (obs_source_active(source) && !it->executable.empty())
				apps.push_back({it->executable, it->title, true});
			obs_source_release(source);
			++it;
		}
	}

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (std::strcmp(obs_source_get_unversioned_id(source), "window_capture") != 0 ||
			    !obs_source_active(source) || obs_source_get_width(source) == 0)
				return true;
			obs_data_t *settings = obs_source_get_settings(source);
			CapturedApp app = parseWindowSetting(obs_data_get_string(settings, "window"));
			obs_data_release(settings);
			if (!app.executable.empty())
				static_cast<std::vector<CapturedApp> *>(param)->push_back(app);
			return true;
		},
		&apps);

	// Gespeicherte Titel sind oft veraltet (Emulatoren ändern ihn, wenn ein Spiel startet)
	for (CapturedApp &app : apps) {
		std::string live = currentWindowTitle(app.executable);
		if (!live.empty())
			app.title = live;
	}
	return apps;
}

} // namespace kgs
