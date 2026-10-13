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

#include "switcher.hpp"

#include "ai-client.hpp"
#include "capture-info.hpp"
#include "frame-grabber.hpp"
#include "http.hpp"
#include "window-hint.hpp"

// obs-module.h muss vor plugin-support.h stehen, sonst kollidiert die blogva-Deklaration
#include <obs-module.h>
#include <plugin-support.h>

#include <QTime>

#include <algorithm>
#include <utility>

namespace kgs {

namespace {

QString qstr(const std::string &value)
{
	return QString::fromStdString(value);
}

bool sameName(const std::string &a, const std::string &b)
{
	return qstr(a).trimmed().compare(qstr(b).trimmed(), Qt::CaseInsensitive) == 0;
}

constexpr auto kManualHold = std::chrono::minutes(15);

// Hinweis für die KI: was OBS aufnimmt und welches Fenster vorne ist
std::string appHint(const std::vector<CapturedApp> &apps, const ForegroundWindow &foreground)
{
	std::string captured;
	for (const CapturedApp &app : apps) {
		captured += (captured.empty() ? "" : "; ") + app.executable + " (window \"" + app.title + "\", " +
			    (app.gameCapture ? "game capture" : "window capture") + ")";
	}
	std::string hint;
	if (!captured.empty())
		hint += "Applications captured by OBS: " + captured + "\n";
	if (!foreground.executable.empty())
		hint += "Active window: \"" + foreground.title + "\" (process: " + foreground.executable + ")\n";
	return hint;
}

// Aufgenommene Programme zuerst – sie sind tatsächlich im Bild; danach das Vordergrundfenster
const GameRule *matchRule(const Settings &cfg, const std::vector<CapturedApp> &apps, const ForegroundWindow &foreground)
{
	auto matches = [](const std::string &executable, const GameRule &rule) {
		return !executable.empty() && sameName(executable, rule.executable);
	};
	for (const CapturedApp &app : apps) {
		for (const GameRule &rule : cfg.rules) {
			if (matches(app.executable, rule))
				return &rule;
		}
	}
	for (const GameRule &rule : cfg.rules) {
		if (matches(foreground.executable, rule))
			return &rule;
	}
	return nullptr;
}

struct BusyGuard {
	std::function<void(bool)> set;
	explicit BusyGuard(std::function<void(bool)> setter) : set(std::move(setter)) { set(true); }
	~BusyGuard() { set(false); }
};

} // namespace

Switcher::Switcher() : settings_(loadSettings()) {}

Switcher::~Switcher()
{
	stop();
}

void Switcher::start()
{
	std::lock_guard lock(mutex_);
	if (started_ || stopping_)
		return;
	started_ = true;
	worker_ = std::thread(&Switcher::run, this);
}

void Switcher::stop()
{
	{
		std::lock_guard lock(mutex_);
		if (stopping_)
			return;
		stopping_ = true;
	}
	cancelLogin_ = true;
	httpAbortAll();
	wake_.notify_all();
	if (worker_.joinable())
		worker_.join();

	std::lock_guard lock(helpersMutex_);
	for (std::thread &helper : helpers_) {
		if (helper.joinable())
			helper.join();
	}
	helpers_.clear();
}

Settings Switcher::settings() const
{
	std::lock_guard lock(mutex_);
	return settings_;
}

void Switcher::setSettings(const Settings &settings)
{
	{
		std::lock_guard lock(mutex_);
		const bool resumed = settings.enabled && !settings_.enabled;
		settings_ = settings;
		if (resumed)
			dueNow_ = true;
		saveSettings(settings_);
	}
	wake_.notify_all();
	emit statusChanged();
}

void Switcher::setEnabled(bool enabled)
{
	Settings changed = settings();
	changed.enabled = enabled;
	setSettings(changed);
	addHistory(enabled ? "Automatik fortgesetzt" : "Automatik pausiert");
}

void Switcher::setStreaming(bool streaming)
{
	{
		std::lock_guard lock(mutex_);
		if (streaming_ == streaming)
			return;
		streaming_ = streaming;
		if (streaming)
			dueNow_ = true; // beim Go-Live sofort prüfen
	}
	if (streaming)
		refreshChannel_ = true;
	wake_.notify_all();
	emit statusChanged();
}

void Switcher::checkNow()
{
	{
		std::lock_guard lock(mutex_);
		request_ = Request::Apply;
	}
	wake_.notify_all();
}

void Switcher::correctCategory(const std::string &category, const std::string &rememberExecutable)
{
	if (!rememberExecutable.empty()) {
		Settings changed = settings();
		auto &rules = changed.rules;
		rules.erase(std::remove_if(rules.begin(), rules.end(),
					   [&](const GameRule &rule) {
						   return sameName(rule.executable, rememberExecutable);
					   }),
			    rules.end());
		rules.push_back({rememberExecutable, category});
		setSettings(changed);
		addHistory("Zuordnung gemerkt: " + qstr(rememberExecutable) + " → " + qstr(category));
	}
	{
		std::lock_guard lock(mutex_);
		manualCategory_ = category;
		request_ = Request::Manual;
	}
	wake_.notify_all();
}

void Switcher::testNow()
{
	{
		std::lock_guard lock(mutex_);
		request_ = Request::Test;
	}
	wake_.notify_all();
}

StatusSnapshot Switcher::status() const
{
	StatusSnapshot s;
	s.twitchConnected = twitch_.connected();
	s.twitchLogin = qstr(twitch_.login());
	s.loginRunning = loginRunning_;

	std::lock_guard lock(mutex_);
	s.enabled = settings_.enabled;
	s.onlyLive = settings_.onlyLive;
	s.streaming = streaming_;
	s.busy = busy_;
	if (busy_)
		s.busySeconds = static_cast<int>(
			std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - busySince_)
				.count());
	if (nextCheck_ && s.enabled && (!s.onlyLive || s.streaming) && s.twitchConnected) {
		auto left = std::chrono::duration_cast<std::chrono::seconds>(*nextCheck_ -
									     std::chrono::steady_clock::now());
		s.secondsUntilCheck = std::max(0, static_cast<int>(left.count()));
	}
	s.deviceCode = deviceCode_;
	s.verificationUri = verificationUri_;
	s.detection = detection_;
	s.detectionReason = detectionReason_;
	s.detectionTime = detectionTime_;
	s.category = category_;
	s.title = title_;
	s.notice = notice_;
	if (holdUntil_ && *holdUntil_ > std::chrono::steady_clock::now()) {
		auto left = std::chrono::duration_cast<std::chrono::minutes>(*holdUntil_ -
									     std::chrono::steady_clock::now());
		s.holdMinutes = static_cast<int>(left.count()) + 1;
	}
	return s;
}

void Switcher::addHistory(const QString &text)
{
	obs_log(LOG_INFO, "%s", text.toUtf8().constData());
	emit historyAdded(QTime::currentTime().toString("HH:mm") + "  " + text);
}

void Switcher::setNotice(const QString &text, bool toHistory)
{
	bool changed;
	{
		std::lock_guard lock(mutex_);
		changed = notice_ != text;
		notice_ = text;
	}
	// Gleiche Meldung nicht bei jeder Prüfung erneut in den Verlauf schreiben
	if (changed) {
		if (toHistory && !text.isEmpty())
			addHistory(text);
		emit statusChanged();
	}
}

void Switcher::setBusy(bool busy)
{
	{
		std::lock_guard lock(mutex_);
		busy_ = busy;
		if (busy)
			busySince_ = std::chrono::steady_clock::now();
	}
	emit statusChanged();
}

void Switcher::resetCandidate()
{
	candidate_.clear();
	candidateHits_ = 0;
}

void Switcher::runInBackground(std::function<void()> task)
{
	std::lock_guard lock(helpersMutex_);
	helpers_.emplace_back(std::move(task));
}

// -- Worker -------------------------------------------------------------------

void Switcher::run()
{
	std::unique_lock lock(mutex_);
	while (!stopping_) {
		wake_.wait_for(lock, std::chrono::seconds(1));
		if (stopping_)
			break;

		const Request request = std::exchange(request_, Request::None);
		const std::string manualCategory = std::exchange(manualCategory_, std::string());
		if (dueNow_) {
			dueNow_ = false;
			nextCheck_ = std::chrono::steady_clock::now();
		}
		const Settings cfg = settings_;
		const bool streaming = streaming_;
		const bool due = !nextCheck_ || std::chrono::steady_clock::now() >= *nextCheck_;
		const bool automatic = cfg.enabled && (!cfg.onlyLive || streaming);
		const bool runCheck = request != Request::None || (automatic && due);
		if (runCheck && request != Request::Test)
			nextCheck_ = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.interval);

		lock.unlock();
		try {
			// Kategorie und Titel für die Anzeige holen (nach Start, Login und Go-Live)
			if (twitch_.connected() && refreshChannel_.exchange(false))
				refreshChannel();
			if (request == Request::Manual)
				applyManual(cfg, manualCategory);
			else if (runCheck)
				check(cfg, request == Request::Test, request == Request::Apply);
		} catch (const std::exception &e) {
			setNotice(QString("Fehler: ") + e.what(), true);
		}
		lock.lock();
	}
}

void Switcher::refreshChannel()
{
	ChannelInfo channel = twitch_.getChannel();
	knownCategory_ = channel.gameName;
	{
		std::lock_guard lock(mutex_);
		category_ = qstr(channel.gameName);
		title_ = qstr(channel.title);
	}
	emit statusChanged();
}

void Switcher::check(const Settings &cfg, bool dryRun, bool immediate)
{
	if (!dryRun && !twitch_.connected()) {
		setNotice("Nicht mit Twitch verbunden – Erkennung pausiert", true);
		return;
	}

	const std::vector<CapturedApp> apps = activeCapturedApps();
	const ForegroundWindow foreground = foregroundWindow();
	std::string target;
	std::string alternative; // zweiter Name für die Kategoriesuche
	bool certain = false;

	if (const GameRule *rule = matchRule(cfg, apps, foreground)) {
		// Feste Zuordnung: sicher, schnell und ohne KI
		target = rule->category;
		certain = true;
		const QString label = qstr(rule->category);
		{
			std::lock_guard lock(mutex_);
			detection_ = label + " (Zuordnung)";
			detectionReason_ = "Feste Zuordnung für " + qstr(rule->executable);
			detectionTime_ = QTime::currentTime().toString("HH:mm:ss");
		}
		setNotice({}, false);
		emit statusChanged();
		if (dryRun || label != lastDetectionLabel_)
			addHistory((dryRun ? "Test: " : "Erkannt: ") + label + " · Zuordnung " +
				   qstr(rule->executable));
		lastDetectionLabel_ = label;
		try {
			const QImage frame = grabFrame(cfg.sourceName, cfg.imageWidth);
			if (!frame.isNull())
				emit previewChanged(frame);
		} catch (const std::exception &) {
			// Vorschau ist hier nur Beiwerk
		}
		if (dryRun)
			return;
	} else {
		QImage frame;
		try {
			frame = grabFrame(cfg.sourceName, cfg.imageWidth);
		} catch (const std::runtime_error &e) {
			setNotice(e.what(), true);
			return;
		}
		if (frame.isNull() || isBlack(frame)) {
			setNotice("Kein Bild (Quelle leer oder schwarz) – übersprungen", true);
			return;
		}
		emit previewChanged(frame);
		const EncodedImage image = encodeImage(frame);
		const std::string hint = cfg.sendWindowHint ? appHint(apps, foreground) : std::string();

		Detection detection;
		const auto started = std::chrono::steady_clock::now();
		{
			BusyGuard busy([this](bool value) { setBusy(value); });
			detection = detectGame(aiTargetFrom(cfg), image, hint);
		}
		const double seconds =
			std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

		const QString label = detection.isGame ? qstr(detection.category) : QStringLiteral("kein Spiel");
		const int percent = qRound(detection.confidence * 100);
		const QString labelWithConfidence = QString("%1 (%2 %)").arg(label).arg(percent);
		QString details = QString(" · %1 s").arg(QString::number(seconds, 'f', 1).replace('.', ','));
		if (detection.model != cfg.activeModel())
			details += " · Ersatzmodell " + qstr(detection.model);
		{
			std::lock_guard lock(mutex_);
			detection_ = labelWithConfidence;
			detectionReason_ = qstr(detection.reason);
			detectionTime_ = QTime::currentTime().toString("HH:mm:ss");
		}
		setNotice({}, false);
		emit statusChanged();
		if (dryRun || label != lastDetectionLabel_)
			addHistory((dryRun ? "Test: " : "Erkannt: ") + labelWithConfidence + details);
		lastDetectionLabel_ = label;

		if (dryRun)
			return;
		if (percent < cfg.minConfidence) {
			resetCandidate();
			return;
		}
		target = detection.isGame ? detection.category : cfg.idleCategory;
		if (detection.isGame)
			alternative = detection.game;
	}

	if (target.empty()) {
		resetCandidate();
		return;
	}

	std::optional<Category> category = twitch_.findCategory(target);
	if (!category && !alternative.empty() && !sameName(alternative, target))
		category = twitch_.findCategory(alternative);
	if (!category) {
		addHistory("Keine Twitch-Kategorie für „" + qstr(target) + "“ gefunden");
		resetCandidate();
		return;
	}
	if (!knownCategory_)
		refreshChannel();
	if (sameName(category->name, *knownCategory_)) {
		if (immediate)
			addHistory("Kategorie ist bereits „" + qstr(category->name) + "“");
		resetCandidate();
		return;
	}

	// Nach einer manuellen Korrektur überschreibt die KI sie eine Weile nicht (Zuordnungen schon)
	if (!immediate && !certain) {
		std::lock_guard lock(mutex_);
		if (holdUntil_ && std::chrono::steady_clock::now() < *holdUntil_) {
			resetCandidate();
			return;
		}
	}

	if (category->name == candidate_) {
		candidateHits_++;
	} else {
		candidate_ = category->name;
		candidateHits_ = 1;
	}
	const int needed = (immediate || certain) ? 1 : cfg.confirmations;
	if (candidateHits_ < needed) {
		addHistory(QString("Wechsel zu „%1“ vorgemerkt (%2/%3)")
				   .arg(qstr(category->name))
				   .arg(candidateHits_)
				   .arg(needed));
		return;
	}
	apply(cfg, *category);
}

void Switcher::applyManual(const Settings &cfg, const std::string &name)
{
	if (!twitch_.connected()) {
		setNotice("Nicht mit Twitch verbunden", true);
		return;
	}
	std::optional<Category> category = twitch_.findCategory(name);
	if (!category) {
		setNotice("Keine Twitch-Kategorie für „" + qstr(name) + "“ gefunden", true);
		return;
	}
	{
		std::lock_guard lock(mutex_);
		holdUntil_ = std::chrono::steady_clock::now() + kManualHold;
		detection_ = qstr(category->name) + " (manuell)";
		detectionReason_.clear();
		detectionTime_ = QTime::currentTime().toString("HH:mm:ss");
	}
	lastDetectionLabel_ = qstr(category->name);
	setNotice({}, false);
	addHistory("Manuell korrigiert: " + qstr(category->name) + " – die KI stellt 15 Minuten nicht um");
	if (!knownCategory_)
		refreshChannel();
	apply(cfg, *category);
}

void Switcher::apply(const Settings &cfg, const Category &category)
{
	resetCandidate();
	ChannelInfo channel = twitch_.getChannel();
	if (channel.gameId == category.id) {
		knownCategory_ = category.name;
		{
			std::lock_guard lock(mutex_);
			category_ = qstr(channel.gameName);
			title_ = qstr(channel.title);
		}
		emit statusChanged();
		return;
	}

	std::string title;
	if (cfg.titleMode == TitleMode::Ai) {
		try {
			BusyGuard busy([this](bool value) { setBusy(value); });
			title = writeTitle(aiTargetFrom(cfg), category.name, cfg.titleStyle, channel.title);
		} catch (const std::exception &e) {
			addHistory(QString("KI-Titel fehlgeschlagen (%1) – nehme die Vorlage").arg(e.what()));
		}
	}
	if (cfg.titleMode != TitleMode::Keep && title.empty())
		title = qstr(cfg.titleTemplate).replace("{game}", qstr(category.name)).trimmed().toStdString();

	twitch_.updateChannel(category.id, title);
	knownCategory_ = category.name;
	{
		std::lock_guard lock(mutex_);
		category_ = qstr(category.name);
		if (!title.empty())
			title_ = qstr(title);
	}
	emit statusChanged();
	addHistory("Twitch umgestellt: " + qstr(category.name) + (title.empty() ? "" : " · " + qstr(title)));
}

// -- Twitch-Login -----------------------------------------------------------------

void Switcher::connectTwitch()
{
	if (loginRunning_.exchange(true))
		return;
	cancelLogin_ = false;
	const std::string clientId = settings().effectiveTwitchClientId();
	emit statusChanged();
	runInBackground([this, clientId] { loginTask(clientId); });
}

void Switcher::cancelTwitchLogin()
{
	cancelLogin_ = true;
}

void Switcher::loginTask(const std::string &clientId)
{
	try {
		const DeviceFlow flow = twitch_.startDeviceFlow(clientId);
		{
			std::lock_guard lock(mutex_);
			deviceCode_ = qstr(flow.userCode);
			verificationUri_ = qstr(flow.verificationUri);
		}
		emit statusChanged();
		emit openUrlRequested(qstr(flow.verificationUri));

		if (twitch_.waitForDeviceLogin(clientId, flow, [this] { return cancelLogin_.load(); })) {
			addHistory("Mit Twitch verbunden als " + qstr(twitch_.login()));
			setNotice({}, false);
			refreshChannel_ = true; // setzt auch die bekannte Kategorie neu
			std::lock_guard lock(mutex_);
			dueNow_ = true;
		} else if (!cancelLogin_) {
			addHistory("Twitch-Anmeldung abgelaufen – bitte erneut verbinden");
		}
	} catch (const std::exception &e) {
		setNotice(e.what(), true);
	}

	{
		std::lock_guard lock(mutex_);
		deviceCode_.clear();
		verificationUri_.clear();
	}
	loginRunning_ = false;
	wake_.notify_all();
	emit statusChanged();
}

void Switcher::disconnectTwitch()
{
	cancelLogin_ = true;
	runInBackground([this] {
		twitch_.disconnect();
		refreshChannel_ = true;
		{
			std::lock_guard lock(mutex_);
			category_.clear();
			title_.clear();
		}
		addHistory("Twitch-Verbindung getrennt");
		emit statusChanged();
	});
}

} // namespace kgs
