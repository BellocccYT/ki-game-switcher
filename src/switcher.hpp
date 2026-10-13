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

#include "ai-client.hpp"
#include "settings.hpp"
#include "twitch.hpp"

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace kgs {

struct StatusSnapshot {
	bool enabled = true;
	bool onlyLive = true;
	bool streaming = false;    // OBS streamt oder Twitch meldet den Kanal als live
	bool liveOnTwitch = false; // live laut Twitch (ohne Streamstart in OBS)
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
	QString scene;       // Spielsituation laut KI, z. B. „Bosskampf gegen …“
	QString speech;      // zuletzt erkannter Satz
	QString voiceStatus; // z. B. „Hört zu: Mic/Aux“
	bool voiceEnabled = false;
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
	void correctCategory(const std::string &category, const std::string &rememberExecutable,
			     const std::string &rememberWindowTitle = {});
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
	void apply(const Settings &cfg, const Category &category, const std::string &scene,
		   const std::vector<std::string> &keywords, const GameRule *rule);
	void maybeProgressTitle(const Settings &cfg, const Detection &detection, const GameRule *rule);
	std::string makeTitle(const Settings &cfg, const std::string &game, const std::string &scene,
			      const std::vector<std::string> &keywords, const std::string &previousTitle);
	std::vector<std::string> speechKeywords(const Settings &cfg, const std::string &game);
	// Ergebnis des Steam-Abgleichs: Name der Kategorie, auf die umgestellt werden darf; leer = abgelehnt
	std::optional<std::string> verifyWithSteam(const Settings &cfg, const EncodedImage &image,
						   const Detection &detection);
	void applyManual(const Settings &cfg, const std::string &name);
	void refreshChannel();
	void pollTwitchLive();
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
	bool streaming_ = false;  // OBS-Streamausgabe läuft
	bool twitchLive_ = false; // Twitch meldet den Kanal als live
	std::optional<std::chrono::steady_clock::time_point> nextLivePoll_;
	bool livePollFailed_ = false;
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
	QString scene_;
	QString speech_;

	// Nur im Worker-Thread benutzt
	std::optional<std::string> knownCategory_;
	std::string candidate_;
	int candidateHits_ = 0;
	QString lastDetectionLabel_;
	std::string lastScene_;    // zuletzt angezeigte Situation (für den Verlauf)
	std::string lastBlocked_;  // zuletzt gemeldete gesperrte Kategorie
	std::string recentSpeech_; // was in den letzten Minuten gesagt wurde (für Stichwörter)
	std::string titleScene_;   // Situation, zu der der aktuelle Titel passt
	int progressHits_ = 0;
	bool progressWaitLogged_ = false;
	std::optional<std::chrono::steady_clock::time_point> lastTitleChange_;
	// Steam-Abgleich je erkanntem Namen: Ergebnis (leer = abgelehnt) und Ablaufzeit
	std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> verdicts_;

	std::atomic<bool> refreshChannel_{true};
	std::atomic<bool> loginRunning_{false};
	std::atomic<bool> cancelLogin_{false};

	Twitch twitch_;
	std::thread worker_;
	std::mutex helpersMutex_;
	std::vector<std::thread> helpers_;
};

} // namespace kgs
