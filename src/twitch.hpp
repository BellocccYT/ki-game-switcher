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

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

class QJsonObject;

namespace kgs {

struct HttpResponse;

class TwitchError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

struct DeviceFlow {
	std::string deviceCode;
	std::string userCode;
	std::string verificationUri;
	int expiresIn = 1800;
	int interval = 5;
};

struct ChannelInfo {
	std::string gameId;
	std::string gameName;
	std::string title;
};

struct Category {
	std::string id;
	std::string name;
};

// Twitch-Login per Device Code Flow und die Helix-Aufrufe, die das Plugin braucht. Thread-sicher.
// Alle Aufrufe blockieren und gehören in Hintergrund-Threads.
class Twitch {
public:
	Twitch();

	bool connected() const;
	std::string login() const;
	void disconnect();

	DeviceFlow startDeviceFlow(const std::string &clientId);
	// true = eingeloggt, false = abgelaufen oder abgebrochen
	bool waitForDeviceLogin(const std::string &clientId, const DeviceFlow &flow,
				const std::function<bool()> &cancelled);

	ChannelInfo getChannel();
	std::optional<Category> findCategory(const std::string &name);
	// tags leer = Tags bleiben, wie sie sind
	void updateChannel(const std::string &gameId, const std::string &title,
			   const std::vector<std::string> &tags = {});
	void updateTitle(const std::string &title);

private:
	void load();
	void save();
	void publishInfo();
	bool validate();
	void refresh();
	QJsonObject helix(const char *method, const std::string &path, const std::string &query,
			  const std::string &body = {});

	mutable std::recursive_mutex mutex_; // schützt Tokens; wird auch während Netzwerkaufrufen gehalten

	// Kopie für connected()/login(), damit die Oberfläche nie auf Netzwerkaufrufe wartet
	mutable std::mutex infoMutex_;
	bool infoConnected_ = false;
	std::string infoLogin_;

	std::string clientId_;
	std::string accessToken_;
	std::string refreshToken_;
	std::string userId_;
	std::string login_;
	std::chrono::steady_clock::time_point validatedAt_{};
	bool validated_ = false;
	std::map<std::string, std::optional<Category>> categoryCache_;
};

} // namespace kgs
