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

#pragma once

#include "twitch.hpp"

#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>

class QJsonObject;

namespace kgs {

class YouTubeError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

constexpr int kYouTubeTitleLimit = 100;

// YouTube-Login per Device Flow (eigenes Google-Cloud-Projekt des Nutzers) und Titeländerung des laufenden
// Livestreams. Die YouTube-API kennt keine Spiel-Kategorie wie Twitch – geändert wird nur der Titel.
// Thread-sicher; alle Aufrufe blockieren und gehören in Hintergrund-Threads.
class YouTube {
public:
	YouTube();

	bool connected() const;
	std::string channel() const;
	void disconnect();

	DeviceFlow startDeviceFlow(const std::string &clientId);
	// true = eingeloggt, false = abgelaufen oder abgebrochen
	bool waitForDeviceLogin(const std::string &clientId, const std::string &clientSecret, const DeviceFlow &flow,
				const std::function<bool()> &cancelled);

	// Läuft gerade ein Livestream auf dem Kanal?
	bool isLive();
	// Setzt den Titel des laufenden Livestreams; false, wenn gerade keiner läuft
	bool updateTitle(const std::string &title);

private:
	void load();
	void save();
	void publishInfo();
	void refresh();
	QJsonObject api(const char *method, const std::string &path, const std::string &query,
			const std::string &body = {});
	std::string activeBroadcast();

	mutable std::recursive_mutex mutex_;
	mutable std::mutex infoMutex_;
	bool infoConnected_ = false;
	std::string infoChannel_;

	std::string clientId_;
	std::string clientSecret_;
	std::string accessToken_;
	std::string refreshToken_;
	std::string channel_;
};

} // namespace kgs
