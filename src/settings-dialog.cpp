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

#include "settings-dialog.hpp"

#include "ai-client.hpp"
#include "capture-info.hpp"
#include "switcher.hpp"
#include "title.hpp"
#include "voice.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QHeaderView>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <thread>

namespace kgs {

namespace {

QPointer<SettingsDialog> g_openDialog;

QString qstr(const std::string &value)
{
	return QString::fromStdString(value);
}

std::string text(const QLineEdit *edit)
{
	return edit->text().trimmed().toStdString();
}

QLabel *hint(const QString &text)
{
	auto *label = new QLabel(text);
	label->setWordWrap(true);
	label->setTextFormat(Qt::RichText);
	label->setOpenExternalLinks(true);
	return label;
}

QFormLayout *formPage(QWidget *page)
{
	auto *outer = new QVBoxLayout(page);
	auto *form = new QFormLayout;
	outer->addLayout(form);
	outer->addStretch(1);
	return form;
}

std::vector<std::string> videoSources()
{
	std::vector<std::string> names;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO)
				static_cast<std::vector<std::string> *>(param)->push_back(obs_source_get_name(source));
			return true;
		},
		&names);
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return qstr(a).compare(qstr(b), Qt::CaseInsensitive) < 0;
	});
	return names;
}

} // namespace

void SettingsDialog::open(Switcher *switcher, QWidget *parent)
{
	if (!switcher)
		return;
	if (!g_openDialog) {
		g_openDialog = new SettingsDialog(switcher, parent);
		g_openDialog->setAttribute(Qt::WA_DeleteOnClose);
	}
	g_openDialog->show();
	g_openDialog->raise();
	g_openDialog->activateWindow();
}

SettingsDialog::SettingsDialog(Switcher *switcher, QWidget *parent)
	: QDialog(parent),
	  switcher_(switcher),
	  settings_(switcher->settings())
{
	setWindowTitle("KI Game Switcher – Einstellungen");
	setMinimumWidth(680);

	auto *tabs = new QTabWidget;
	tabs->addTab(createAiTab(), "KI");
	tabs->addTab(createTwitchTab(), "Twitch");
	tabs->addTab(createYouTubeTab(), "YouTube");
	tabs->addTab(createDetectionTab(), "Erkennung");
	tabs->addTab(createTitleTab(), "Titel");
	tabs->addTab(createVoiceTab(), "Stimme");
	tabs->addTab(createRulesTab(), "Zuordnungen");

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Save)->setText("Speichern");
	buttons->button(QDialogButtonBox::Cancel)->setText("Abbrechen");
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);
	layout->addWidget(buttons);

	connect(switcher, &Switcher::statusChanged, this, &SettingsDialog::refreshTwitch);
	connect(switcher, &Switcher::statusChanged, this, &SettingsDialog::refreshYouTube);
	refreshTwitch();
	refreshYouTube();

	// Download-Fortschritt und erkannter Text der Spracherkennung
	auto *voiceTimer = new QTimer(this);
	connect(voiceTimer, &QTimer::timeout, this, &SettingsDialog::refreshVoice);
	voiceTimer->start(1000);
	refreshVoice();
}

QWidget *SettingsDialog::createAiTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	provider_ = new QComboBox;
	for (Provider provider : providerDisplayOrder())
		provider_->addItem(providerInfo(provider).label, static_cast<int>(provider));
	form->addRow("Anbieter:", provider_);

	providerHelp_ = hint({});
	form->addRow(providerHelp_);

	apiKey_ = new QLineEdit;
	apiKey_->setEchoMode(QLineEdit::Password);
	showKey_ = new QCheckBox("anzeigen");
	auto *keyRow = new QHBoxLayout;
	keyRow->addWidget(apiKey_, 1);
	keyRow->addWidget(showKey_);
	form->addRow("API-Key:", keyRow);
	connect(showKey_, &QCheckBox::toggled, this,
		[this](bool show) { apiKey_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password); });

	model_ = new QComboBox;
	model_->setEditable(true);
	model_->setInsertPolicy(QComboBox::NoInsert);
	model_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	loadModels_ = new QPushButton("Modelle laden");
	loadModels_->setToolTip("Fragt beim Anbieter ab, welche Modelle du nutzen kannst – prüft dabei auch den Key");
	auto *modelRow = new QHBoxLayout;
	modelRow->addWidget(model_, 1);
	modelRow->addWidget(loadModels_);
	form->addRow("Modell:", modelRow);
	fallbackModel_ = new QComboBox;
	fallbackModel_->setEditable(true);
	fallbackModel_->setInsertPolicy(QComboBox::NoInsert);
	fallbackModel_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	fallbackModel_->lineEdit()->setPlaceholderText("keins");
	fallbackModel_->setToolTip("Springt ein, wenn das Hauptmodell überlastet ist, sein Limit erreicht hat oder "
				   "nicht antwortet.");
	form->addRow("Ersatzmodell:", fallbackModel_);

	modelStatus_ = hint({});
	form->addRow(modelStatus_);
	connect(loadModels_, &QPushButton::clicked, this, &SettingsDialog::loadModels);

	baseUrl_ = new QLineEdit;
	form->addRow("Server-URL:", baseUrl_);

	connect(provider_, &QComboBox::currentIndexChanged, this, [this] {
		storeProvider(shownProvider_);
		showProvider(provider_->currentData().toInt());
	});
	provider_->setCurrentIndex(std::max(0, provider_->findData(static_cast<int>(settings_.provider))));
	showProvider(provider_->currentData().toInt());
	return page;
}

QWidget *SettingsDialog::createTwitchTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	twitchStatus_ = new QLabel;
	twitchButton_ = new QPushButton;
	auto *row = new QHBoxLayout;
	row->addWidget(twitchStatus_, 1);
	row->addWidget(twitchButton_);
	form->addRow("Konto:", row);
	connect(twitchButton_, &QPushButton::clicked, this, [this] {
		if (!switcher_)
			return;
		const StatusSnapshot s = switcher_->status();
		if (s.loginRunning) {
			switcher_->cancelTwitchLogin();
		} else if (s.twitchConnected) {
			switcher_->disconnectTwitch();
		} else {
			// Eine geänderte Client-ID muss vor dem Login gespeichert sein
			Settings current = switcher_->settings();
			current.twitchClientId = text(clientId_);
			switcher_->setSettings(current);
			settings_.twitchClientId = current.twitchClientId;
			switcher_->connectTwitch();
		}
	});

	form->addRow(hint("Beim Verbinden öffnet sich Twitch im Browser. Dort den angezeigten Code bestätigen – "
			  "das Plugin darf danach nur Titel und Kategorie deines Kanals ändern."));

	clientId_ = new QLineEdit(qstr(settings_.twitchClientId));
	clientId_->setPlaceholderText("leer = Twitch-App des Plugins");
	form->addRow("Eigene Client-ID:", clientId_);
	form->addRow(hint("Nur nötig, wenn du eine eigene App auf "
			  "<a href=\"https://dev.twitch.tv/console/apps\">dev.twitch.tv</a> nutzen willst "
			  "(Client-Typ „Öffentlich“)."));
	twitchTags_ = new QLineEdit(qstr(settings_.twitchTags));
	twitchTags_->setPlaceholderText("z. B. Deutsch, Gaming – leer = Tags nicht ändern");
	twitchTags_->setToolTip("Werden bei jedem Spielwechsel gesetzt, zusammen mit den Tags der Zuordnung. "
				"Twitch erlaubt höchstens 10 Tags aus Buchstaben und Ziffern (max. 25 Zeichen). "
				"Leer lassen, wenn du deine Tags selbst pflegst.");
	form->addRow("Tags bei jedem Wechsel:", twitchTags_);
	return page;
}

QWidget *SettingsDialog::createDetectionTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	enabled_ = new QCheckBox("Automatisch prüfen und umstellen");
	enabled_->setChecked(settings_.enabled);
	form->addRow(enabled_);

	onlyLive_ = new QCheckBox("Nur prüfen, während du live bist");
	onlyLive_->setToolTip("Live heißt: OBS streamt oder Twitch meldet deinen Kanal als live (wird jede Minute "
			      "abgefragt). So klappt es auch, wenn du nicht über „Stream starten“ in OBS live gehst.");
	onlyLive_->setChecked(settings_.onlyLive);
	form->addRow(onlyLive_);

	interval_ = new QSpinBox;
	interval_->setRange(20, 3600);
	interval_->setSingleStep(10);
	interval_->setSuffix(" s");
	interval_->setValue(settings_.interval);
	form->addRow("Prüfintervall:", interval_);

	source_ = new QComboBox;
	source_->addItem("Programmausgabe (was die Zuschauer sehen)", QString());
	for (const std::string &name : videoSources())
		source_->addItem(qstr(name), qstr(name));
	if (!settings_.sourceName.empty() && source_->findData(qstr(settings_.sourceName)) < 0)
		source_->addItem(qstr(settings_.sourceName) + " (nicht gefunden)", qstr(settings_.sourceName));
	source_->setCurrentIndex(std::max(0, source_->findData(qstr(settings_.sourceName))));
	source_->setToolTip("Eine einzelne Quelle wie deine Spielaufnahme ist genauer, weil Webcam und Overlays "
			    "die KI dann nicht ablenken.");
	form->addRow("Bildquelle:", source_);

	imageWidth_ = new QSpinBox;
	imageWidth_->setRange(512, 1280);
	imageWidth_->setSingleStep(64);
	imageWidth_->setSuffix(" px");
	imageWidth_->setValue(settings_.imageWidth);
	imageWidth_->setToolTip("Größer erkennt Details besser, braucht aber mehr Daten pro Prüfung.");
	form->addRow("Bildbreite:", imageWidth_);

	minConfidence_ = new QSpinBox;
	minConfidence_->setRange(0, 100);
	minConfidence_->setSuffix(" %");
	minConfidence_->setValue(settings_.minConfidence);
	form->addRow("Mindest-Sicherheit:", minConfidence_);

	confirmations_ = new QSpinBox;
	confirmations_->setRange(1, 10);
	confirmations_->setValue(settings_.confirmations);
	confirmations_->setToolTip("So oft hintereinander muss das neue Spiel erkannt werden. Verhindert Wechsel, "
				   "wenn du nur kurz in ein anderes Fenster schaust.");
	form->addRow("Bestätigungen:", confirmations_);

	idleCategory_ = new QLineEdit(qstr(settings_.idleCategory));
	idleCategory_->setPlaceholderText("z. B. Just Chatting – leer = Kategorie bleibt");
	form->addRow("Ohne Spiel:", idleCategory_);

	windowHint_ = new QCheckBox("Programm- und Fensternamen als Hinweis mitsenden");
	windowHint_->setChecked(settings_.sendWindowHint);
	windowHint_->setToolTip("Schickt mit, welche Programme OBS aufnimmt und welches Fenster aktiv ist. Verbessert "
				"die Erkennung deutlich, Fenstertitel können aber private Infos enthalten (z. B. "
				"Browser-Tabs).");
	form->addRow(windowHint_);

	verifyDatabase_ = new QCheckBox("Vor dem Umstellen mit Steam-Screenshots abgleichen");
	verifyDatabase_->setChecked(settings_.verifyWithDatabase);
	verifyDatabase_->setToolTip(
		"Bevor die KI auf ein anderes Spiel umstellt, holt das Plugin Screenshots der "
		"infrage kommenden Spiele aus dem Steam-Shop und lässt die KI vergleichen. Genauer, "
		"dauert pro Wechsel aber einige Sekunden länger. Spiele, die es nicht bei Steam gibt, "
		"werden ohne Abgleich umgestellt.");
	form->addRow(verifyDatabase_);

	blocked_ = new QLineEdit(qstr(settings_.blockedCategories));
	blocked_->setPlaceholderText("z. B. Just Chatting, Bloodborne");
	blocked_->setToolTip("Auf diese Kategorien stellt die KI nie um – praktisch für Spiele, mit denen sie dein "
			     "Spiel oft verwechselt. Feste Zuordnungen gelten trotzdem.");
	form->addRow("Nie umstellen auf:", blocked_);
	return page;
}

QWidget *SettingsDialog::createTitleTab()
{
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);

	// -- Wie der Titel entsteht
	auto *how = new QGroupBox("So entsteht der Titel");
	auto *form = new QFormLayout(how);
	titleMode_ = new QComboBox;
	titleMode_->addItem("Von der KI schreiben lassen", static_cast<int>(TitleMode::Ai));
	titleMode_->addItem("Vorlage verwenden", static_cast<int>(TitleMode::Template));
	titleMode_->addItem("Titel nicht ändern", static_cast<int>(TitleMode::Keep));
	titleMode_->setCurrentIndex(std::max(0, titleMode_->findData(static_cast<int>(settings_.titleMode))));
	form->addRow("Titel:", titleMode_);

	titlePreset_ = new QComboBox;
	for (const TitlePreset &preset : titlePresets()) {
		titlePreset_->addItem(preset.label, QString(preset.id));
		titlePreset_->setItemData(titlePreset_->count() - 1, QString::fromUtf8(preset.style), Qt::ToolTipRole);
	}
	titlePreset_->setCurrentIndex(std::max(0, titlePreset_->findData(qstr(settings_.titlePreset))));
	form->addRow("Stil:", titlePreset_);

	titleLanguage_ = new QComboBox;
	titleLanguage_->addItem("Deutsch", QStringLiteral("de"));
	titleLanguage_->addItem("Englisch", QStringLiteral("en"));
	titleLanguage_->setCurrentIndex(std::max(0, titleLanguage_->findData(qstr(settings_.titleLanguage))));
	titleEmojis_ = new QCheckBox("Emojis erlauben");
	titleEmojis_->setChecked(settings_.titleEmojis);
	titleEmojis_->setToolTip(
		"Aus: Das Plugin entfernt Emojis selbst aus jedem Titel – auch aus der Vorlage und "
		"egal, was das Modell schreibt. Deine festen Teile vorne und hinten bleiben unverändert.");
	auto *languageRow = new QHBoxLayout;
	languageRow->addWidget(titleLanguage_);
	languageRow->addWidget(titleEmojis_);
	languageRow->addStretch(1);
	form->addRow("Sprache:", languageRow);

	titleStyle_ = new QPlainTextEdit(qstr(settings_.titleStyle));
	titleStyle_->setMaximumHeight(60);
	titleStyle_->setPlaceholderText("Optional, z. B. „Erwähne, dass es ein Blind-Run ist“");
	form->addRow("Eigene Ergänzungen:", titleStyle_);
	form->addRow(hint("Der Stil ist eine fertige Vorgabe – du musst keine lange Anweisung schreiben. Immer gilt "
			  "zusätzlich: gewählte Sprache, eine Zeile, keine Hashtags, nichts Erfundenes, höchstens 140 "
			  "Zeichen."));

	titleTemplate_ = new QLineEdit(qstr(settings_.titleTemplate));
	form->addRow("Vorlage:", titleTemplate_);
	form->addRow(hint("Platzhalter: <code>{game}</code> = Twitch-Kategorie, <code>{scene}</code> = Situation im "
			  "Spiel (z. B. „Bosskampf gegen den Enderdrachen“), <code>{keywords}</code> = Stichwörter aus "
			  "deiner Stimme. Leere Platzhalter verschwinden samt Trennzeichen davor. Die Vorlage springt "
			  "auch ein, wenn der KI-Titel fehlschlägt."));
	outer->addWidget(how);

	// -- Feste Vorgaben
	auto *fixed = new QGroupBox("Feste Vorgaben – gelten immer, egal welches Modell");
	auto *fixedForm = new QFormLayout(fixed);
	titlePrefix_ = new QLineEdit(qstr(settings_.titlePrefix));
	titlePrefix_->setPlaceholderText("z. B. [DE]");
	fixedForm->addRow("Immer vorne:", titlePrefix_);
	titleSuffix_ = new QLineEdit(qstr(settings_.titleSuffix));
	titleSuffix_->setPlaceholderText("z. B. | !discord !socials");
	fixedForm->addRow("Immer hinten:", titleSuffix_);
	fixedForm->addRow(hint("Das Plugin setzt diese Teile selbst an jeden Titel – die KI kann sie nicht vergessen "
			       "oder umschreiben. Zu lange Titel kürzt es in der Mitte."));
	titlePreview_ = hint(QString());
	titlePreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	fixedForm->addRow("Vorschau:", titlePreview_);
	outer->addWidget(fixed);

	// -- Spielfortschritt
	auto *progress = new QGroupBox("Spielfortschritt");
	auto *progressForm = new QFormLayout(progress);
	progressTitles_ = new QCheckBox("Titel auch anpassen, wenn sich im Spiel etwas Wichtiges ändert");
	progressTitles_->setChecked(settings_.progressTitles);
	progressTitles_->setToolTip("Neues Level, neue Welt, Bosskampf, anderer Spielmodus … Die KI muss den neuen "
				    "Abschnitt so oft hintereinander erkennen, wie unter „Erkennung → Bestätigungen“ "
				    "eingestellt.");
	progressForm->addRow(progressTitles_);
	progressLevel_ = new QComboBox;
	progressLevel_->addItem("Selten – nur große Abschnitte (neues Level, Bosskampf, neue Welt)",
				QStringLiteral("big"));
	progressLevel_->addItem("Normal – auch neue Gebiete, Runden, Siege und Niederlagen", QStringLiteral("normal"));
	progressLevel_->addItem("Häufig – bei jeder deutlich anderen Situation", QStringLiteral("often"));
	progressLevel_->setCurrentIndex(std::max(0, progressLevel_->findData(qstr(settings_.progressLevel))));
	progressForm->addRow("Wie oft:", progressLevel_);
	progressMinutes_ = new QSpinBox;
	progressMinutes_->setRange(2, 240);
	progressMinutes_->setSingleStep(1);
	progressMinutes_->setSuffix(" min");
	progressMinutes_->setValue(settings_.progressMinutes);
	progressForm->addRow("Frühestens alle:", progressMinutes_);
	outer->addWidget(progress);
	outer->addStretch(1);

	auto updateEnabled = [this] {
		const auto mode = static_cast<TitleMode>(titleMode_->currentData().toInt());
		titlePreset_->setEnabled(mode == TitleMode::Ai);
		titleStyle_->setEnabled(mode == TitleMode::Ai);
		titleTemplate_->setEnabled(mode != TitleMode::Keep);
		progressTitles_->setEnabled(mode != TitleMode::Keep);
		progressMinutes_->setEnabled(mode != TitleMode::Keep && progressTitles_->isChecked());
		progressLevel_->setEnabled(mode != TitleMode::Keep && progressTitles_->isChecked());
		updateTitlePreview();
	};
	connect(titleMode_, &QComboBox::currentIndexChanged, this, updateEnabled);
	connect(progressTitles_, &QCheckBox::toggled, this, updateEnabled);
	connect(titleTemplate_, &QLineEdit::textChanged, this, &SettingsDialog::updateTitlePreview);
	connect(titleEmojis_, &QCheckBox::toggled, this, &SettingsDialog::updateTitlePreview);
	connect(titlePrefix_, &QLineEdit::textChanged, this, &SettingsDialog::updateTitlePreview);
	connect(titleSuffix_, &QLineEdit::textChanged, this, &SettingsDialog::updateTitlePreview);
	updateEnabled();
	return page;
}

void SettingsDialog::updateTitlePreview()
{
	Settings example = settings_;
	example.titlePrefix = text(titlePrefix_);
	example.titleSuffix = text(titleSuffix_);
	example.titleEmojis = titleEmojis_->isChecked();
	const auto mode = static_cast<TitleMode>(titleMode_->currentData().toInt());
	if (mode == TitleMode::Keep) {
		titlePreview_->setText("<i>Der Titel bleibt, wie er ist.</i>");
		return;
	}
	const std::string core = mode == TitleMode::Ai ? std::string("‹Titel von der KI›")
						       : fillTemplate(titleTemplate_->text().toStdString(), "Minecraft",
								      "Bosskampf gegen den Enderdrachen", {"Hardcore"});
	titlePreview_->setText(qstr(composeTitle(example, core)).toHtmlEscaped());
}

QWidget *SettingsDialog::createYouTubeTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);
	form->addRow(hint(
		"Das Plugin ändert bei YouTube den <b>Titel</b> deines Livestreams (eine Spiel-Kategorie wie bei Twitch "
		"bietet die YouTube-Schnittstelle nicht an). Dafür brauchst du einmalig ein eigenes, kostenloses "
		"Google-Cloud-Projekt:<ol>"
		"<li><a href=\"https://console.cloud.google.com/projectcreate\">Projekt anlegen</a> (Name egal)</li>"
		"<li><a href=\"https://console.cloud.google.com/apis/library/youtube.googleapis.com\">YouTube Data API v3</a> "
		"aktivieren</li>"
		"<li><a href=\"https://console.cloud.google.com/auth/overview\">OAuth-Zustimmungsbildschirm</a> einrichten: "
		"Typ <b>Extern</b>, und unter <b>Zielgruppe → Testnutzer</b> dein eigenes Google-Konto eintragen</li>"
		"<li><a href=\"https://console.cloud.google.com/auth/clients\">Client erstellen</a>: Typ <b>Fernseher "
		"und Geräte mit begrenzter Eingabe</b></li>"
		"<li>Client-ID und Clientschlüssel unten eintragen, speichern und auf <b>Mit YouTube verbinden</b> "
		"klicken</li></ol>"
		"Google zeigt beim Verbinden einen Hinweis „App nicht überprüft“ – das ist bei eigenen Projekten normal "
		"(<b>Weiter</b>). Das Tageslimit deines Projekts reicht für rund 150 Titeländerungen."));

	youtubeClientId_ = new QLineEdit(qstr(settings_.youtubeClientId));
	youtubeClientId_->setPlaceholderText("….apps.googleusercontent.com");
	form->addRow("Client-ID:", youtubeClientId_);
	youtubeClientSecret_ = new QLineEdit(qstr(settings_.youtubeClientSecret));
	youtubeClientSecret_->setEchoMode(QLineEdit::Password);
	form->addRow("Clientschlüssel:", youtubeClientSecret_);

	youtubeStatus_ = new QLabel;
	youtubeButton_ = new QPushButton;
	auto *row = new QHBoxLayout;
	row->addWidget(youtubeStatus_, 1);
	row->addWidget(youtubeButton_);
	form->addRow("Konto:", row);
	connect(youtubeButton_, &QPushButton::clicked, this, [this] {
		if (!switcher_)
			return;
		const StatusSnapshot s = switcher_->status();
		if (s.youtubeLoginRunning) {
			switcher_->cancelYouTubeLogin();
		} else if (s.youtubeConnected) {
			switcher_->disconnectYouTube();
		} else {
			// Zugangsdaten zuerst übernehmen, damit der Login sie nutzt
			Settings changed = switcher_->settings();
			changed.youtubeClientId = text(youtubeClientId_);
			changed.youtubeClientSecret = text(youtubeClientSecret_);
			if (changed.youtubeClientId.empty() || changed.youtubeClientSecret.empty()) {
				QMessageBox::information(this, windowTitle(),
							 "Bitte zuerst Client-ID und Clientschlüssel eintragen.");
				return;
			}
			settings_.youtubeClientId = changed.youtubeClientId;
			settings_.youtubeClientSecret = changed.youtubeClientSecret;
			switcher_->setSettings(changed);
			switcher_->connectYouTube();
		}
	});
	return page;
}

void SettingsDialog::refreshYouTube()
{
	if (!switcher_)
		return;
	const StatusSnapshot s = switcher_->status();
	if (s.youtubeLoginRunning) {
		youtubeStatus_->setText(s.loginService == "YouTube" && !s.deviceCode.isEmpty()
						? "Im Browser den Code " + s.deviceCode + " bestätigen …"
						: QString("Anmeldung läuft …"));
		youtubeButton_->setText("Abbrechen");
	} else if (s.youtubeConnected) {
		youtubeStatus_->setText("Verbunden: " + s.youtubeChannel);
		youtubeButton_->setText("Trennen");
	} else {
		youtubeStatus_->setText("Nicht verbunden");
		youtubeButton_->setText("Mit YouTube verbinden");
	}
}

QWidget *SettingsDialog::createVoiceTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	std::string reason;
	const bool supported = voice::supported(&reason);
	voiceEnabled_ = new QCheckBox("Stimme mithören und Stichwörter für den Titel nutzen");
	voiceEnabled_->setChecked(settings_.voiceEnabled && supported);
	voiceEnabled_->setEnabled(supported);
	form->addRow(voiceEnabled_);
	form->addRow(hint(
		supported
			? "Die Spracherkennung läuft komplett auf deinem PC (whisper.cpp). Nichts wird "
			  "aufgenommen oder gespeichert: Das Plugin hält nur die letzten zwei Minuten Audio im "
			  "Arbeitsspeicher und merkt sich den erkannten Text der letzten drei Minuten. Wird ein neuer "
			  "Titel geschrieben, geht dieser Text an die gewählte KI – bei Ollama bleibt also alles "
			  "lokal. Auf die Spielerkennung hat das Gesagte keinen Einfluss."
			: "<b>" + qstr(reason) + "</b>"));

	voiceSource_ = new QComboBox;
	voiceSource_->addItem("Automatisch (erstes Mikrofon)", QString());
	for (const voice::AudioSource &source : voice::audioSources()) {
		QString label = qstr(source.name);
		if (!source.device.empty())
			label += " – " + qstr(source.device);
		if (source.global)
			label += " (OBS-Audiogerät)";
		voiceSource_->addItem(label, qstr(source.name));
	}
	if (!settings_.voiceSource.empty() && voiceSource_->findData(qstr(settings_.voiceSource)) < 0) {
		// Auch andere Audioquellen sind erlaubt (z. B. ein eigener Mikrofon-Filter-Mix)
		obs_source_t *source = obs_get_source_by_name(settings_.voiceSource.c_str());
		voiceSource_->addItem(qstr(settings_.voiceSource) + (source ? "" : " (nicht gefunden)"),
				      qstr(settings_.voiceSource));
		obs_source_release(source);
	}
	voiceSource_->setCurrentIndex(std::max(0, voiceSource_->findData(qstr(settings_.voiceSource))));
	form->addRow("Mikrofon:", voiceSource_);

	voiceModel_ = new QComboBox;
	for (const VoiceModelInfo &model : voiceModels())
		voiceModel_->addItem(model.label, QString(model.id));
	voiceModel_->setCurrentIndex(std::max(0, voiceModel_->findData(qstr(settings_.voiceModel))));
	form->addRow("Sprachmodell:", voiceModel_);
	form->addRow(hint("Wird beim ersten Einschalten einmalig von huggingface.co heruntergeladen. Die Erkennung "
			  "läuft auf dem Prozessor, nur während einer Prüfung und dauert dann ein paar Sekunden."));

	voiceStatus_ = hint(QString());
	form->addRow("Status:", voiceStatus_);
	voiceText_ = hint(QString());
	voiceText_->setTextFormat(Qt::PlainText);
	form->addRow("Zuletzt verstanden:", voiceText_);

	auto updateEnabled = [this] {
		voiceSource_->setEnabled(voiceEnabled_->isChecked());
		voiceModel_->setEnabled(voiceEnabled_->isChecked());
	};
	connect(voiceEnabled_, &QCheckBox::toggled, this, updateEnabled);
	updateEnabled();
	return page;
}

void SettingsDialog::refreshVoice()
{
	voiceStatus_->setText(qstr(voice::status()).toHtmlEscaped());
	const std::string last = voice::lastText();
	voiceText_->setText(last.empty() ? QStringLiteral("–") : qstr(last));
}

QWidget *SettingsDialog::createRulesTab()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout(page);
	layout->addWidget(
		hint("Nimmt OBS eines dieser Programme auf (Spiel- oder Fensteraufnahme) oder ist es das "
		     "aktive Fenster, stellt das Plugin sofort und ohne KI auf die Kategorie um – 100 % "
		     "sicher und kostenlos.<br><b>Emulatoren:</b> Trag dasselbe Programm mehrmals ein, jeweils "
		     "mit einem Teil des Fenstertitels (z. B. <code>Dolphin.exe</code> + „Mario Kart“ und "
		     "<code>Dolphin.exe</code> + „Sunshine“). Zeilen mit Fenstertitel gehen vor.<br>"
		     "<b>Eigener Titel</b> (optional, Platzhalter wie in der Vorlage) und <b>Tags</b> gelten nur "
		     "für dieses Spiel."));

	rules_ = new QTableWidget(0, 5);
	rules_->setHorizontalHeaderLabels(
		{"Programm (.exe)", "Fenstertitel enthält", "Twitch-Kategorie", "Eigener Titel", "Tags"});
	rules_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	rules_->horizontalHeader()->setStretchLastSection(true);
	rules_->setColumnWidth(0, 120);
	rules_->setColumnWidth(1, 130);
	rules_->setColumnWidth(2, 140);
	rules_->setColumnWidth(3, 160);
	rules_->verticalHeader()->setVisible(false);
	rules_->setSelectionBehavior(QAbstractItemView::SelectRows);
	rules_->horizontalHeaderItem(1)->setToolTip("Leer = gilt für jedes Fenster des Programms. Bei Emulatoren "
						    "ein Teil des Spielnamens aus dem Fenstertitel.");
	rules_->horizontalHeaderItem(3)->setToolTip("Leer = Titel wie unter „Titel“ eingestellt. Platzhalter: "
						    "{game}, {scene}, {keywords}");
	rules_->horizontalHeaderItem(4)->setToolTip("Twitch-Tags für dieses Spiel, durch Komma getrennt, z. B. "
						    "Speedrun, Nintendo");
	for (const GameRule &rule : settings_.rules)
		addRule(rule);
	layout->addWidget(rules_, 1);

	auto *buttons = new QHBoxLayout;
	auto *add = new QPushButton("Hinzufügen");
	auto *fromCapture = new QPushButton("Aktuelle Aufnahmen übernehmen");
	fromCapture->setToolTip("Trägt die Programme ein, die OBS gerade aufnimmt. Bei Emulatoren wird der Spielname "
				"aus dem Fenstertitel vorgeschlagen – bitte prüfen.");
	auto *remove = new QPushButton("Entfernen");
	buttons->addWidget(add);
	buttons->addWidget(fromCapture);
	buttons->addStretch(1);
	buttons->addWidget(remove);
	layout->addLayout(buttons);

	connect(add, &QPushButton::clicked, this, [this] {
		addRule({});
		rules_->editItem(rules_->item(rules_->rowCount() - 1, 0));
	});
	connect(fromCapture, &QPushButton::clicked, this, [this] {
		for (const CapturedApp &app : activeCapturedApps()) {
			GameRule rule;
			rule.executable = app.executable;
			if (isEmulator(app.executable)) {
				rule.windowTitle = gameFromWindowTitle(app.title);
				rule.category = rule.windowTitle;
			}
			bool exists = false;
			for (int row = 0; row < rules_->rowCount(); row++) {
				auto same = [&](int column, const std::string &value) {
					return rules_->item(row, column) &&
					       rules_->item(row, column)
							       ->text()
							       .trimmed()
							       .compare(qstr(value), Qt::CaseInsensitive) == 0;
				};
				exists = exists || (same(0, rule.executable) && same(1, rule.windowTitle));
			}
			if (!exists)
				addRule(rule);
		}
	});
	connect(remove, &QPushButton::clicked, this, [this] {
		const int row = rules_->currentRow();
		if (row >= 0)
			rules_->removeRow(row);
	});
	return page;
}

void SettingsDialog::addRule(const GameRule &rule)
{
	const int row = rules_->rowCount();
	rules_->insertRow(row);
	const std::string values[] = {rule.executable, rule.windowTitle, rule.category, rule.title, rule.tags};
	for (int column = 0; column < 5; column++)
		rules_->setItem(row, column, new QTableWidgetItem(qstr(values[column])));
}

void SettingsDialog::showProvider(int index)
{
	if (index < 0 || index >= kProviderCount)
		return;
	shownProvider_ = index;
	const Provider provider = static_cast<Provider>(index);
	const ProviderInfo &info = providerInfo(provider);
	const ProviderSettings &values = settings_.providers[index];

	providerHelp_->setText(info.help);
	apiKey_->setText(qstr(values.apiKey));
	apiKey_->setEnabled(!isLocalProvider(provider));
	apiKey_->setPlaceholderText(info.needsKey ? "erforderlich" : "optional");

	model_->clear();
	model_->setEditText(qstr(values.model.empty() ? info.defaultModel : values.model));
	fallbackModel_->clear();
	fallbackModel_->setEditText(qstr(values.fallbackModel));
	baseUrl_->setText(qstr(values.baseUrl));
	baseUrl_->setPlaceholderText(*info.baseUrl ? info.baseUrl : "z. B. http://localhost:1234/v1");
	modelStatus_->clear();
}

void SettingsDialog::storeProvider(int index)
{
	if (index < 0 || index >= kProviderCount)
		return;
	ProviderSettings &values = settings_.providers[index];
	values.apiKey = text(apiKey_);
	const std::string model = model_->currentText().trimmed().toStdString();
	values.model = model == providerInfo(static_cast<Provider>(index)).defaultModel ? std::string() : model;
	values.fallbackModel = fallbackModel_->currentText().trimmed().toStdString();
	values.baseUrl = text(baseUrl_);
}

void SettingsDialog::loadModels()
{
	storeProvider(shownProvider_);
	Settings probe = settings_;
	probe.provider = static_cast<Provider>(shownProvider_);
	const AiTarget target = aiTargetFrom(probe);

	loadModels_->setEnabled(false);
	modelStatus_->setText("Frage Modelle ab …");
	QPointer<SettingsDialog> self(this);
	std::thread([self, target] {
		std::vector<std::string> models;
		QString error;
		try {
			models = listModels(target);
		} catch (const std::exception &e) {
			error = e.what();
		}
		QMetaObject::invokeMethod(
			qApp,
			[self, models, error] {
				if (self)
					self->setModels(models, error);
			},
			Qt::QueuedConnection);
	}).detach();
}

void SettingsDialog::setModels(const std::vector<std::string> &models, const QString &error)
{
	loadModels_->setEnabled(true);
	if (!error.isEmpty()) {
		modelStatus_->setText("<span style=\"color:#f85149\">" + error.toHtmlEscaped() + "</span>");
		return;
	}
	for (QComboBox *box : {model_, fallbackModel_}) {
		const QString current = box->currentText();
		box->clear();
		for (const std::string &model : models)
			box->addItem(qstr(model));
		box->setEditText(current);
	}
	modelStatus_->setText(models.empty()
				      ? "Verbindung klappt, aber der Anbieter meldet keine passenden Modelle."
				      : QString("Verbindung klappt – %1 Modelle zur Auswahl.").arg(models.size()));
}

void SettingsDialog::refreshTwitch()
{
	if (!switcher_)
		return;
	const StatusSnapshot s = switcher_->status();
	if (s.loginRunning) {
		twitchStatus_->setText(s.deviceCode.isEmpty() ? "Anmeldung läuft …"
							      : "Code im Browser bestätigen: " + s.deviceCode);
		twitchButton_->setText("Abbrechen");
	} else if (s.twitchConnected) {
		twitchStatus_->setText("Verbunden als " + s.twitchLogin);
		twitchButton_->setText("Trennen");
	} else {
		twitchStatus_->setText("Nicht verbunden");
		twitchButton_->setText("Mit Twitch verbinden");
	}
}

void SettingsDialog::save()
{
	if (!switcher_) {
		reject();
		return;
	}
	storeProvider(shownProvider_);
	settings_.provider = static_cast<Provider>(shownProvider_);
	settings_.twitchClientId = text(clientId_);

	settings_.enabled = enabled_->isChecked();
	settings_.onlyLive = onlyLive_->isChecked();
	settings_.interval = interval_->value();
	settings_.sourceName = source_->currentData().toString().toStdString();
	settings_.imageWidth = imageWidth_->value();
	settings_.minConfidence = minConfidence_->value();
	settings_.confirmations = confirmations_->value();
	settings_.idleCategory = text(idleCategory_);
	settings_.sendWindowHint = windowHint_->isChecked();

	settings_.verifyWithDatabase = verifyDatabase_->isChecked();
	settings_.blockedCategories = text(blocked_);
	settings_.twitchTags = text(twitchTags_);
	settings_.titleLanguage = titleLanguage_->currentData().toString().toStdString();
	settings_.titleEmojis = titleEmojis_->isChecked();

	settings_.titleMode = static_cast<TitleMode>(titleMode_->currentData().toInt());
	settings_.titlePreset = titlePreset_->currentData().toString().toStdString();
	settings_.titleTemplate = titleTemplate_->text().toStdString();
	settings_.titleStyle = titleStyle_->toPlainText().trimmed().toStdString();
	settings_.titlePrefix = text(titlePrefix_);
	settings_.titleSuffix = text(titleSuffix_);
	settings_.progressTitles = progressTitles_->isChecked();
	settings_.progressMinutes = progressMinutes_->value();
	settings_.progressLevel = progressLevel_->currentData().toString().toStdString();
	settings_.youtubeClientId = text(youtubeClientId_);
	settings_.youtubeClientSecret = text(youtubeClientSecret_);

	settings_.voiceEnabled = voiceEnabled_->isChecked();
	settings_.voiceSource = voiceSource_->currentData().toString().toStdString();
	settings_.voiceModel = voiceModel_->currentData().toString().toStdString();

	settings_.rules.clear();
	auto cell = [this](int row, int column) {
		return rules_->item(row, column) ? rules_->item(row, column)->text().trimmed().toStdString()
						 : std::string();
	};
	for (int row = 0; row < rules_->rowCount(); row++) {
		GameRule rule;
		rule.executable = cell(row, 0);
		rule.windowTitle = cell(row, 1);
		rule.category = cell(row, 2);
		rule.title = cell(row, 3);
		rule.tags = cell(row, 4);
		if (!rule.executable.empty() && !rule.category.empty())
			settings_.rules.push_back(rule);
	}

	const ProviderInfo &info = providerInfo(settings_.provider);
	if (info.needsKey && settings_.activeProvider().apiKey.empty()) {
		QMessageBox::warning(this, windowTitle(),
				     QString("Für „%1“ brauchst du einen API-Key.").arg(info.label));
		return;
	}
	if (settings_.activeBaseUrl().empty()) {
		QMessageBox::warning(this, windowTitle(), "Bitte eine Server-URL eintragen.");
		return;
	}

	switcher_->setSettings(settings_);
	accept();
}

} // namespace kgs
