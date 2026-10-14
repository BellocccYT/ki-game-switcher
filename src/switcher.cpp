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

#include "switcher.hpp"

#include "ai-client.hpp"
#include "capture-info.hpp"
#include "frame-grabber.hpp"
#include "game-db.hpp"
#include "http.hpp"
#include "title.hpp"
#include "voice.hpp"
#include "window-hint.hpp"

// obs-module.h muss vor plugin-support.h stehen, sonst kollidiert die blogva-Deklaration
#include <obs-module.h>
#include <plugin-support.h>

#include <QStringList>
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
constexpr auto kLivePoll = std::chrono::seconds(60);      // so oft fragt das Plugin Twitch, ob du live bist
constexpr auto kVerdictValid = std::chrono::minutes(10);  // bestätigtes Steam-Ergebnis
constexpr auto kRejectionValid = std::chrono::minutes(3); // abgelehntes Steam-Ergebnis

std::string lowerKey(const std::string &value)
{
	return qstr(value).trimmed().toLower().toStdString();
}

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

// Aufgenommene Programme zuerst – sie sind tatsächlich im Bild; danach das Vordergrundfenster.
// Zuordnungen mit Fenstertitel (Emulatoren: mehrere Spiele pro Programm) gehen allgemeinen vor.
const GameRule *matchRule(const Settings &cfg, const std::vector<CapturedApp> &apps, const ForegroundWindow &foreground)
{
	auto matches = [](const std::string &executable, const std::string &title, const GameRule &rule) {
		if (executable.empty() || !sameName(executable, rule.executable))
			return false;
		const QString part = qstr(rule.windowTitle).trimmed();
		return part.isEmpty() || qstr(title).contains(part, Qt::CaseInsensitive);
	};
	std::vector<std::pair<std::string, std::string>> windows;
	for (const CapturedApp &app : apps)
		windows.emplace_back(app.executable, app.title);
	windows.emplace_back(foreground.executable, foreground.title);

	for (const auto &[executable, title] : windows) {
		for (const bool specific : {true, false}) {
			for (const GameRule &rule : cfg.rules) {
				if (qstr(rule.windowTitle).trimmed().isEmpty() != specific &&
				    matches(executable, title, rule))
					return &rule;
			}
		}
	}
	return nullptr;
}

bool isBlocked(const Settings &cfg, const std::string &category)
{
	for (const std::string &blocked : splitList(cfg.blockedCategories)) {
		if (sameName(blocked, category))
			return true;
	}
	return false;
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
	cancelYouTubeLogin_ = true;
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
		nextLivePoll_.reset();
		settings_ = settings;
		if (resumed)
			dueNow_ = true;
		saveSettings(settings_);
	}
	voice::configure(settings);
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

void Switcher::correctCategory(const std::string &category, const std::string &rememberExecutable,
			       const std::string &rememberWindowTitle)
{
	if (!rememberExecutable.empty()) {
		Settings changed = settings();
		auto &rules = changed.rules;
		// Gleiches Programm mit gleichem Fenstertitel-Filter ersetzen, andere Spiele des Emulators behalten
		rules.erase(std::remove_if(rules.begin(), rules.end(),
					   [&](const GameRule &rule) {
						   return sameName(rule.executable, rememberExecutable) &&
							  sameName(rule.windowTitle, rememberWindowTitle);
					   }),
			    rules.end());
		GameRule rule;
		rule.executable = rememberExecutable;
		rule.windowTitle = rememberWindowTitle;
		rule.category = category;
		rules.push_back(rule);
		setSettings(changed);
		addHistory("Zuordnung gemerkt: " + qstr(rememberExecutable) +
			   (rememberWindowTitle.empty() ? QString() : " („" + qstr(rememberWindowTitle) + "“)") +
			   " → " + qstr(category));
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
	s.loginRunning = loginRunning_ || youtubeLoginRunning_;
	s.youtubeConnected = youtube_.connected();
	s.youtubeChannel = qstr(youtube_.channel());
	s.youtubeLoginRunning = youtubeLoginRunning_;

	std::lock_guard lock(mutex_);
	s.enabled = settings_.enabled;
	s.onlyLive = settings_.onlyLive;
	s.streaming = streaming_ || twitchLive_;
	s.liveOnTwitch = twitchLive_ && !streaming_;
	s.liveSource = liveSource_;
	s.loginService = loginService_;
	s.busy = busy_;
	if (busy_)
		s.busySeconds = static_cast<int>(
			std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - busySince_)
				.count());
	if (nextCheck_ && s.enabled && (!s.onlyLive || s.streaming) && (s.twitchConnected || s.youtubeConnected)) {
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
	s.scene = scene_;
	s.speech = speech_;
	s.voiceEnabled = settings_.voiceEnabled;
	s.voiceStatus = qstr(voice::status());
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

		// Nicht jeder streamt über den Knopf in OBS – deshalb auch Twitch fragen, ob der Kanal live ist
		if (settings_.enabled && settings_.onlyLive && !streaming_ &&
		    (!nextLivePoll_ || std::chrono::steady_clock::now() >= *nextLivePoll_)) {
			nextLivePoll_ = std::chrono::steady_clock::now() + kLivePoll;
			lock.unlock();
			pollTwitchLive();
			lock.lock();
			if (stopping_)
				break;
		}

		const Request request = std::exchange(request_, Request::None);
		const std::string manualCategory = std::exchange(manualCategory_, std::string());
		if (dueNow_) {
			dueNow_ = false;
			nextCheck_ = std::chrono::steady_clock::now();
		}
		const Settings cfg = settings_;
		const bool streaming = streaming_ || twitchLive_;
		const bool due = !nextCheck_ || std::chrono::steady_clock::now() >= *nextCheck_;
		const bool automatic = cfg.enabled && (!cfg.onlyLive || streaming);
		const bool runCheck = request != Request::None || (automatic && due);
		if (runCheck && request != Request::Test)
			nextCheck_ = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.interval);

		lock.unlock();
		voice::configure(cfg);
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

void Switcher::pollTwitchLive()
{
	bool live = false;
	QString source;
	bool failed = false;
	try {
		if (twitch_.connected() && twitch_.isLive()) {
			live = true;
			source = "Twitch";
		}
	} catch (const std::exception &e) {
		if (!livePollFailed_)
			addHistory(QString("Twitch-Livestatus nicht abrufbar: %1").arg(e.what()));
		failed = true;
	}
	try {
		if (!live && youtube_.connected() && youtube_.isLive()) {
			live = true;
			source = "YouTube";
		}
	} catch (const std::exception &e) {
		if (!livePollFailed_)
			addHistory(QString("YouTube-Livestatus nicht abrufbar: %1").arg(e.what()));
		failed = true;
	}
	livePollFailed_ = failed;
	if (failed && !live)
		return;

	bool changed;
	{
		std::lock_guard lock(mutex_);
		changed = live != twitchLive_;
		twitchLive_ = live;
		if (live)
			liveSource_ = source;
		if (changed && live && !streaming_)
			dueNow_ = true; // wie beim Streamstart in OBS: sofort prüfen
	}
	if (!changed)
		return;
	if (live)
		refreshChannel_ = true;
	addHistory(live ? source + " meldet: Du bist live – Erkennung läuft" : QString("Stream beendet"));
	emit statusChanged();
}

void Switcher::refreshChannel()
{
	if (!twitch_.connected()) {
		// Nur YouTube: Die Kategorie merkt sich das Plugin selbst
		if (!knownCategory_)
			knownCategory_ = std::string();
		return;
	}
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
	if (!dryRun && !twitch_.connected() && !youtube_.connected()) {
		setNotice("Weder mit Twitch noch mit YouTube verbunden – Erkennung pausiert", true);
		return;
	}

	const std::vector<CapturedApp> apps = activeCapturedApps();
	const ForegroundWindow foreground = foregroundWindow();
	const GameRule *rule = matchRule(cfg, apps, foreground);
	// Für Fortschritts-Titel braucht es die KI auch bei festen Zuordnungen (sie beschreibt die Situation)
	const bool wantScene = cfg.progressTitles && cfg.titleMode != TitleMode::Keep;

	QImage frame;
	try {
		frame = grabFrame(cfg.sourceName, cfg.imageWidth);
	} catch (const std::runtime_error &e) {
		if (!rule) {
			setNotice(e.what(), true);
			return;
		}
	}
	if (!frame.isNull())
		emit previewChanged(frame);
	const bool haveFrame = !frame.isNull() && !isBlack(frame);
	if (!rule && !haveFrame) {
		setNotice("Kein Bild (Quelle leer oder schwarz) – übersprungen", true);
		return;
	}

	// Gesprochenes seit der letzten Prüfung (lokal mit whisper.cpp)
	std::string speech;
	if (cfg.voiceEnabled) {
		BusyGuard busy([this](bool value) { setBusy(value); });
		speech = voice::listen(knownCategory_.value_or(std::string()));
		recentSpeech_ = speech;
		std::lock_guard lock(mutex_);
		speech_ = qstr(voice::lastText());
	}

	Detection detection;
	bool haveDetection = false;
	EncodedImage image;
	QString details;
	if (haveFrame && (!rule || wantScene)) {
		image = encodeImage(frame);
		DetectContext context;
		context.appHint = cfg.sendWindowHint ? appHint(apps, foreground) : std::string();
		context.previousGame = knownCategory_.value_or(std::string());
		context.language = titleLanguageName(cfg);
		const auto started = std::chrono::steady_clock::now();
		try {
			BusyGuard busy([this](bool value) { setBusy(value); });
			detection = detectGame(aiTargetFrom(cfg), image, context);
			haveDetection = true;
		} catch (const std::exception &e) {
			if (!rule)
				throw;
			addHistory(QString("Situation nicht erkannt: %1").arg(e.what()));
		}
		const double seconds =
			std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		details = QString(" · %1 s").arg(QString::number(seconds, 'f', 1).replace('.', ','));
		if (haveDetection && detection.model != cfg.activeModel())
			details += " · Ersatzmodell " + qstr(detection.model);
	}
	const int percent = haveDetection ? qRound(detection.confidence * 100) : 0;
	const bool sure = haveDetection && percent >= cfg.minConfidence;

	std::string target;
	std::string alternative; // zweiter Name für die Kategoriesuche
	bool certain = false;

	if (rule) {
		// Feste Zuordnung: sicher, schnell und ohne KI
		target = rule->category;
		certain = true;
		const QString label = qstr(rule->category);
		{
			std::lock_guard lock(mutex_);
			detection_ = label + " (Zuordnung)";
			detectionReason_ = "Feste Zuordnung für " + qstr(rule->executable);
			detectionTime_ = QTime::currentTime().toString("HH:mm:ss");
			scene_ = sure ? qstr(detection.scene) : QString();
		}
		setNotice({}, false);
		emit statusChanged();
		if (dryRun || label != lastDetectionLabel_)
			addHistory((dryRun ? "Test: " : "Erkannt: ") + label + " · Zuordnung " +
				   qstr(rule->executable) +
				   (rule->windowTitle.empty() ? QString() : " („" + qstr(rule->windowTitle) + "“)"));
		lastDetectionLabel_ = label;
	} else {
		const QString label = detection.isGame ? qstr(detection.category) : QStringLiteral("kein Spiel");
		const QString labelWithConfidence = QString("%1 (%2 %)").arg(label).arg(percent);
		{
			std::lock_guard lock(mutex_);
			detection_ = labelWithConfidence;
			detectionReason_ = qstr(detection.reason);
			detectionTime_ = QTime::currentTime().toString("HH:mm:ss");
			scene_ = qstr(detection.scene);
		}
		setNotice({}, false);
		emit statusChanged();
		if (dryRun || label != lastDetectionLabel_)
			addHistory((dryRun ? "Test: " : "Erkannt: ") + labelWithConfidence + details);
		lastDetectionLabel_ = label;
	}

	// Situation und Stichwörter im Verlauf (nur wenn sich etwas ändert)
	if (haveDetection && sure && !detection.scene.empty() && (dryRun || detection.scene != lastScene_)) {
		QString line = "Situation: " + qstr(detection.scene);
		if (!detection.activity.empty())
			line += " · " + activityLabel(detection.activity);
		addHistory(line);
		lastScene_ = detection.scene;
	}
	if (dryRun && !speech.empty()) {
		QStringList words;
		for (const std::string &keyword :
		     speechKeywords(cfg, haveDetection && detection.isGame ? detection.category : std::string()))
			words << qstr(keyword);
		addHistory("Stichwörter aus der Stimme: " + (words.isEmpty() ? QStringLiteral("–") : words.join(", ")));
	}

	if (dryRun) {
		// Im Test zeigen, was der Steam-Abgleich sagen würde
		if (!rule && haveDetection && detection.isGame && cfg.verifyWithDatabase)
			verifyWithSteam(cfg, image, detection);
		return;
	}

	if (!rule) {
		if (percent < cfg.minConfidence) {
			resetCandidate();
			progressHits_ = 0;
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

	std::optional<Category> category = resolveCategory(target);
	if (!category && !alternative.empty() && !sameName(alternative, target))
		category = resolveCategory(alternative);
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
		lastBlocked_.clear();
		if (haveDetection && sure)
			maybeProgressTitle(cfg, detection, rule);
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

	// Gesperrte Kategorien: darauf stellt die KI nie um (feste Zuordnungen schon)
	if (!certain && isBlocked(cfg, category->name)) {
		if (lastBlocked_ != category->name)
			addHistory("„" + qstr(category->name) + "“ ist gesperrt – kein Wechsel");
		lastBlocked_ = category->name;
		resetCandidate();
		return;
	}

	// Vor einem Wechsel per KI: mit Screenshots aus dem Steam-Shop gegenprüfen
	if (!certain && detection.isGame && cfg.verifyWithDatabase) {
		const std::optional<std::string> verdict = verifyWithSteam(cfg, image, detection);
		if (verdict && verdict->empty()) {
			resetCandidate();
			return;
		}
		if (verdict && !sameName(*verdict, category->name)) {
			std::optional<Category> replacement = resolveCategory(*verdict);
			if (replacement)
				category = replacement;
			if (sameName(category->name, *knownCategory_)) {
				resetCandidate();
				return;
			}
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
	apply(cfg, *category, sure ? detection.scene : std::string(), speechKeywords(cfg, category->name),
	      certain ? rule : nullptr);
}

std::optional<std::string> Switcher::verifyWithSteam(const Settings &cfg, const EncodedImage &image,
						     const Detection &detection)
{
	const std::string main = detection.category.empty() ? detection.game : detection.category;
	const auto now = std::chrono::steady_clock::now();
	auto known = verdicts_.find(lowerKey(main));
	if (known != verdicts_.end()) {
		if (now < known->second.second)
			return known->second.first;
		verdicts_.erase(known);
	}

	// Erst das erkannte Spiel, bei Ablehnung die Alternativen der KI
	std::vector<std::string> names{main};
	for (const std::string &alternative : detection.alternatives) {
		if (names.size() < 3 && std::none_of(names.begin(), names.end(), [&](const std::string &name) {
			    return sameName(name, alternative);
		    }))
			names.push_back(alternative);
	}

	std::optional<std::string> result;
	for (size_t i = 0; i < names.size(); i++) {
		std::optional<ReferenceGame> reference = steamReference(names[i]);
		if (!reference && i == 0 && !detection.game.empty() && !sameName(detection.game, main))
			reference = steamReference(detection.game);
		if (!reference) {
			// Ohne Steam-Bilder (z. B. Konsolenspiel) lässt sich das erkannte Spiel nicht prüfen
			if (i == 0)
				return std::nullopt;
			continue;
		}

		Verification verification;
		const auto started = std::chrono::steady_clock::now();
		try {
			BusyGuard busy([this](bool value) { setBusy(value); });
			verification = verifyGame(aiTargetFrom(cfg), image, encodeImage(referenceSheet(*reference)),
						  reference->name);
		} catch (const std::exception &e) {
			addHistory(QString("Steam-Abgleich übersprungen: %1").arg(e.what()));
			return std::nullopt;
		}
		const QString seconds =
			QString::number(
				std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), 'f',
				1)
				.replace('.', ',');
		addHistory(
			QString("Steam-Abgleich „%1“: %2 · %3 s")
				.arg(qstr(reference->name), verification.sameGame ? "passt" : "passt nicht", seconds));
		if (verification.sameGame) {
			result = names[i];
			break;
		}
	}

	if (!result) {
		addHistory("Kein Wechsel – das Bild passt zu keinem der vermuteten Spiele");
		result = std::string();
	}
	verdicts_[lowerKey(main)] = {*result, now + (result->empty() ? kRejectionValid : kVerdictValid)};
	return result;
}

void Switcher::maybeProgressTitle(const Settings &cfg, const Detection &detection, const GameRule *rule)
{
	if (!cfg.progressTitles || cfg.titleMode == TitleMode::Keep || !detection.isGame || detection.scene.empty()) {
		progressHits_ = 0;
		return;
	}
	if (titleScene_.empty()) {
		// Ausgangspunkt merken – verglichen wird ab der nächsten Prüfung
		titleScene_ = detection.scene;
		titleActivity_ = detection.activity;
		return;
	}
	if (!isNewSection(cfg.progressLevel, titleActivity_, detection.activity)) {
		progressHits_ = 0;
		progressWaitLogged_ = false;
		return;
	}
	progressHits_++;
	// „Häufig“ reagiert sofort, sonst wie beim Spielwechsel erst nach Bestätigungen
	const int needed = cfg.progressLevel == "often" ? 1 : cfg.confirmations;
	if (progressHits_ < needed) {
		addHistory(QString("Neuer Abschnitt „%1“ vorgemerkt (%2/%3)")
				   .arg(qstr(detection.scene))
				   .arg(progressHits_)
				   .arg(needed));
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	const auto minimum = std::chrono::minutes(cfg.progressMinutes);
	if (lastTitleChange_ && now - *lastTitleChange_ < minimum) {
		if (!progressWaitLogged_) {
			const auto left =
				std::chrono::duration_cast<std::chrono::minutes>(minimum - (now - *lastTitleChange_));
			addHistory(QString("Neuer Abschnitt „%1“ – Titel frühestens in %2 min")
					   .arg(qstr(detection.scene))
					   .arg(left.count() + 1));
			progressWaitLogged_ = true;
		}
		return;
	}

	ChannelInfo channel;
	if (twitch_.connected()) {
		channel = twitch_.getChannel();
	} else {
		channel.gameName = knownCategory_.value_or(std::string());
		std::lock_guard lock(mutex_);
		channel.title = title_.toStdString();
	}
	const std::vector<std::string> keywords = speechKeywords(cfg, channel.gameName);
	const std::string core =
		rule && !qstr(rule->title).trimmed().isEmpty()
			? fillTemplate(rule->title, channel.gameName, detection.scene, keywords)
			: makeTitleCore(cfg, channel.gameName, detection.scene, keywords, channel.title);
	const std::string title = core.empty() ? std::string() : composeTitle(cfg, core);
	progressHits_ = 0;
	progressWaitLogged_ = false;
	titleScene_ = detection.scene;
	titleActivity_ = detection.activity;
	lastTitleChange_ = now;
	if (title.empty() || title == channel.title)
		return;
	if (twitch_.connected())
		twitch_.updateTitle(title);
	publishYouTube(composeTitle(cfg, core, kYouTubeTitleLimit));
	{
		std::lock_guard lock(mutex_);
		title_ = qstr(title);
	}
	emit statusChanged();
	addHistory("Titel angepasst (neuer Abschnitt): " + qstr(title));
}

void Switcher::applyManual(const Settings &cfg, const std::string &name)
{
	if (!twitch_.connected() && !youtube_.connected()) {
		setNotice("Weder mit Twitch noch mit YouTube verbunden", true);
		return;
	}
	std::optional<Category> category = resolveCategory(name);
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
	apply(cfg, *category, {}, {}, nullptr);
}

std::vector<std::string> Switcher::speechKeywords(const Settings &cfg, const std::string &game)
{
	if (!cfg.voiceEnabled || recentSpeech_.empty() || cfg.titleMode == TitleMode::Keep)
		return {};
	try {
		BusyGuard busy([this](bool value) { setBusy(value); });
		return extractKeywords(aiTargetFrom(cfg), recentSpeech_, game.empty() ? std::string("-") : game,
				       titleLanguageName(cfg));
	} catch (const std::exception &e) {
		addHistory(QString("Stichwörter aus der Stimme fehlgeschlagen: %1").arg(e.what()));
		return {};
	}
}

std::string Switcher::makeTitleCore(const Settings &cfg, const std::string &game, const std::string &scene,
				    const std::vector<std::string> &keywords, const std::string &previousTitle)
{
	if (cfg.titleMode == TitleMode::Keep)
		return {};
	std::string core;
	if (cfg.titleMode == TitleMode::Ai) {
		TitleRequest request;
		request.game = game;
		request.scene = scene;
		request.keywords = keywords;
		request.style = titleStyleFor(cfg);
		request.previousTitle = previousTitle;
		request.maxLength = coreTitleLimit(cfg);
		request.language = titleLanguageName(cfg);
		request.emojis = cfg.titleEmojis;
		if (!qstr(cfg.titlePrefix).trimmed().isEmpty())
			request.fixedParts += "am Anfang „" + cfg.titlePrefix + "“";
		if (!qstr(cfg.titleSuffix).trimmed().isEmpty())
			request.fixedParts += std::string(request.fixedParts.empty() ? "" : ", ") + "am Ende „" +
					      cfg.titleSuffix + "“";
		try {
			BusyGuard busy([this](bool value) { setBusy(value); });
			core = writeTitle(aiTargetFrom(cfg), request);
		} catch (const std::exception &e) {
			addHistory(QString("KI-Titel fehlgeschlagen (%1) – nehme die Vorlage").arg(e.what()));
		}
	}
	if (core.empty())
		core = fillTemplate(cfg.titleTemplate, game, scene, keywords);
	return core;
}

std::optional<Category> Switcher::resolveCategory(const std::string &name)
{
	if (twitch_.connected())
		return twitch_.findCategory(name);
	// Nur YouTube: Es gibt keine Kategorien zum Nachschlagen, der erkannte Name genügt
	if (qstr(name).trimmed().isEmpty())
		return std::nullopt;
	return Category{"", qstr(name).trimmed().toStdString()};
}

void Switcher::publishYouTube(const std::string &title)
{
	if (!youtube_.connected() || title.empty())
		return;
	try {
		if (youtube_.updateTitle(title)) {
			youtubeNoLiveLogged_ = false;
			addHistory("YouTube-Titel: " + qstr(title));
		} else if (!youtubeNoLiveLogged_) {
			addHistory("YouTube: Gerade kein Livestream – Titel nicht geändert");
			youtubeNoLiveLogged_ = true;
		}
	} catch (const std::exception &e) {
		addHistory(QString("YouTube: %1").arg(e.what()));
	}
}

void Switcher::apply(const Settings &cfg, const Category &category, const std::string &scene,
		     const std::vector<std::string> &keywords, const GameRule *rule)
{
	resetCandidate();
	ChannelInfo channel;
	if (twitch_.connected()) {
		channel = twitch_.getChannel();
	} else {
		channel.gameName = knownCategory_.value_or(std::string());
		std::lock_guard lock(mutex_);
		channel.title = title_.toStdString();
	}
	if (twitch_.connected() ? channel.gameId == category.id : sameName(channel.gameName, category.name)) {
		knownCategory_ = category.name;
		{
			std::lock_guard lock(mutex_);
			category_ = qstr(channel.gameName);
			title_ = qstr(channel.title);
		}
		emit statusChanged();
		return;
	}

	std::string core;
	if (rule && !qstr(rule->title).trimmed().isEmpty() && cfg.titleMode != TitleMode::Keep)
		core = fillTemplate(rule->title, category.name, scene, keywords);
	else
		core = makeTitleCore(cfg, category.name, scene, keywords, channel.title);
	const std::string title = core.empty() ? std::string() : composeTitle(cfg, core);

	std::vector<std::string> tags = splitList(cfg.twitchTags);
	if (rule) {
		for (const std::string &tag : splitList(rule->tags))
			tags.push_back(tag);
	}
	tags = cleanTags(tags);
	if (twitch_.connected())
		twitch_.updateChannel(category.id, title, tags);
	else
		tags.clear();
	if (!core.empty())
		publishYouTube(composeTitle(cfg, core, kYouTubeTitleLimit));
	knownCategory_ = category.name;
	titleScene_ = scene;
	titleActivity_.clear(); // nächste Erkennung setzt den Ausgangspunkt
	progressHits_ = 0;
	progressWaitLogged_ = false;
	lastTitleChange_ = std::chrono::steady_clock::now();
	{
		std::lock_guard lock(mutex_);
		category_ = qstr(category.name);
		if (!title.empty())
			title_ = qstr(title);
	}
	emit statusChanged();
	QStringList tagNames;
	for (const std::string &tag : tags)
		tagNames << qstr(tag);
	addHistory(QString(twitch_.connected() ? "Twitch umgestellt: " : "Spiel gewechselt: ") + qstr(category.name) +
		   (title.empty() ? "" : " · " + qstr(title)) +
		   (tagNames.isEmpty() ? QString() : " · Tags: " + tagNames.join(", ")));
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
			loginService_ = "Twitch";
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

void Switcher::connectYouTube()
{
	if (youtubeLoginRunning_.exchange(true))
		return;
	cancelYouTubeLogin_ = false;
	const Settings cfg = settings();
	emit statusChanged();
	runInBackground([this, cfg] { youtubeLoginTask(cfg.youtubeClientId, cfg.youtubeClientSecret); });
}

void Switcher::cancelYouTubeLogin()
{
	cancelYouTubeLogin_ = true;
}

void Switcher::youtubeLoginTask(const std::string &clientId, const std::string &clientSecret)
{
	try {
		if (clientSecret.empty())
			throw YouTubeError("Kein Client-Secret eingetragen (Einstellungen → YouTube)");
		const DeviceFlow flow = youtube_.startDeviceFlow(clientId);
		{
			std::lock_guard lock(mutex_);
			loginService_ = "YouTube";
			deviceCode_ = qstr(flow.userCode);
			verificationUri_ = qstr(flow.verificationUri);
		}
		emit statusChanged();
		emit openUrlRequested(qstr(flow.verificationUri));
		if (youtube_.waitForDeviceLogin(clientId, clientSecret, flow,
						[this] { return cancelYouTubeLogin_.load(); })) {
			addHistory("Mit YouTube verbunden: " + qstr(youtube_.channel()));
			setNotice({}, false);
			std::lock_guard lock(mutex_);
			nextLivePoll_.reset();
			dueNow_ = true;
		} else if (!cancelYouTubeLogin_) {
			addHistory("YouTube-Anmeldung abgelaufen – bitte erneut verbinden");
		}
	} catch (const std::exception &e) {
		setNotice(e.what(), true);
	}
	{
		std::lock_guard lock(mutex_);
		deviceCode_.clear();
		verificationUri_.clear();
	}
	youtubeLoginRunning_ = false;
	wake_.notify_all();
	emit statusChanged();
}

void Switcher::disconnectYouTube()
{
	cancelYouTubeLogin_ = true;
	runInBackground([this] {
		youtube_.disconnect();
		addHistory("YouTube-Verbindung getrennt");
		emit statusChanged();
	});
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
