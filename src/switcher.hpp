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

#include "settings.hpp"
#include "twitch.hpp"

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace kgs {

struct StatusSnapshot {
	bool enabled = true;
	bool onlyLive = true;
	bool streaming = false;
	bool busy = false;
	int busySeconds = 0;        // wie lange die laufende KI-Anfrage schon dauert
	int secondsUntilCheck = -1; // -1 = keine Prüfung geplant
	bool twitchConnected = false;
	QString twitchLogin;
	bool loginRunning = false;
	QString deviceCode;
	QString verificationUri;
	QString detection; // z. B. „Elden Ring (95 %)“
	QString detectionReason;
	QString detectionTime;
	QString category;    // aktuelle Twitch-Kategorie
	QString title;       // aktueller Twitch-Titel
	QString notice;      // letzter Fehler oder Hinweis
	int holdMinutes = 0; // nach manueller Korrektur: so lange stellt die Automatik nicht um
};

// Herzstück des Plugins: prüft in einem Hintergrund-Thread regelmäßig das Streambild und stellt Twitch um.
// Öffentliche Methoden sind aus dem UI-Thread aufrufbar und blockieren nie auf Netzwerkzugriffe.
class Switcher : public QObject {
	Q_OBJECT

public:
	Switcher();
	~Switcher() override;

	void start();
	void stop();

	Settings settings() const;
	void setSettings(const Settings &settings);
	void setEnabled(bool enabled);
	void setStreaming(bool streaming);

	void checkNow(); // sofort prüfen und ohne Bestätigungsrunden umstellen
	void testNow();  // nur erkennen, nichts ändern
	// Setzt die Kategorie von Hand; mit rememberExecutable wird daraus eine feste Zuordnung
	void correctCategory(const std::string &category, const std::string &rememberExecutable);
	void connectTwitch();
	void cancelTwitchLogin();
	void disconnectTwitch();

	StatusSnapshot status() const;

signals:
	void statusChanged();
	void historyAdded(const QString &entry);
	void previewChanged(const QImage &image);
	void openUrlRequested(const QString &url);

private:
	enum class Request { None, Apply, Test, Manual };

	void run();
	void check(const Settings &cfg, bool dryRun, bool immediate);
	void apply(const Settings &cfg, const Category &category);
	void applyManual(const Settings &cfg, const std::string &name);
	void refreshChannel();
	void loginTask(const std::string &clientId);
	void runInBackground(std::function<void()> task);

	void addHistory(const QString &text);
	void setNotice(const QString &text, bool toHistory);
	void setBusy(bool busy);
	void resetCandidate();

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	Settings settings_;
	bool started_ = false;
	bool stopping_ = false;
	Request request_ = Request::None;
	std::string manualCategory_;
	std::optional<std::chrono::steady_clock::time_point> holdUntil_;
	bool dueNow_ = true;
	bool streaming_ = false;
	std::optional<std::chrono::steady_clock::time_point> nextCheck_;

	// Anzeige-Status, geschützt durch mutex_
	bool busy_ = false;
	std::chrono::steady_clock::time_point busySince_{};
	QString deviceCode_;
	QString verificationUri_;
	QString detection_;
	QString detectionReason_;
	QString detectionTime_;
	QString category_;
	QString title_;
	QString notice_;

	// Nur im Worker-Thread benutzt
	std::optional<std::string> knownCategory_;
	std::string candidate_;
	int candidateHits_ = 0;
	QString lastDetectionLabel_;

	std::atomic<bool> refreshChannel_{true};
	std::atomic<bool> loginRunning_{false};
	std::atomic<bool> cancelLogin_{false};

	Twitch twitch_;
	std::thread worker_;
	std::mutex helpersMutex_;
	std::vector<std::thread> helpers_;
};

} // namespace kgs
