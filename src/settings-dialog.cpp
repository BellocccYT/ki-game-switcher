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

#include "settings-dialog.hpp"
#include "i18n.hpp"

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
	setWindowTitle(Tr("Settings.TitleSyncSettings"));
	setMinimumWidth(680);

	auto *tabs = new QTabWidget;
	tabs->addTab(createAiTab(), Tr("Settings.AI"));
	tabs->addTab(createTwitchTab(), "Twitch");
	tabs->addTab(createYouTubeTab(), "YouTube");
	tabs->addTab(createDetectionTab(), Tr("Settings.Detection"));
	tabs->addTab(createTitleTab(), Tr("Settings.Title"));
	tabs->addTab(createVoiceTab(), Tr("Settings.Voice"));
	tabs->addTab(createRulesTab(), Tr("Settings.Rules"));

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Save)->setText(Tr("Settings.Save"));
	buttons->button(QDialogButtonBox::Cancel)->setText(Tr("Settings.Cancel"));
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Sprache der Oberfläche; gilt für neue Fenster sofort, für das Dock nach einem Neustart von OBS
	uiLanguage_ = new QComboBox;
	uiLanguage_->addItem(Tr("Settings.UiLanguageAuto"), QStringLiteral("auto"));
	uiLanguage_->addItem(QStringLiteral("Deutsch"), QStringLiteral("de"));
	uiLanguage_->addItem(QStringLiteral("English"), QStringLiteral("en"));
	uiLanguage_->setCurrentIndex(std::max(0, uiLanguage_->findData(qstr(settings_.uiLanguage))));
	auto *bottom = new QHBoxLayout;
	bottom->addWidget(new QLabel(Tr("Settings.UiLanguage")));
	bottom->addWidget(uiLanguage_);
	bottom->addStretch();
	bottom->addWidget(buttons);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);
	layout->addLayout(bottom);

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
		provider_->addItem(Tr(providerInfo(provider).label), static_cast<int>(provider));
	form->addRow(Tr("Settings.Provider"), provider_);

	providerHelp_ = hint({});
	form->addRow(providerHelp_);

	apiKey_ = new QLineEdit;
	apiKey_->setEchoMode(QLineEdit::Password);
	showKey_ = new QCheckBox("anzeigen");
	auto *keyRow = new QHBoxLayout;
	keyRow->addWidget(apiKey_, 1);
	keyRow->addWidget(showKey_);
	form->addRow(Tr("Settings.APIKey"), keyRow);
	connect(showKey_, &QCheckBox::toggled, this,
		[this](bool show) { apiKey_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password); });

	model_ = new QComboBox;
	model_->setEditable(true);
	model_->setInsertPolicy(QComboBox::NoInsert);
	model_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	loadModels_ = new QPushButton(Tr("Settings.LoadModels"));
	loadModels_->setToolTip(Tr("Settings.AsksProviderWhichModelsYou"));
	auto *modelRow = new QHBoxLayout;
	modelRow->addWidget(model_, 1);
	modelRow->addWidget(loadModels_);
	form->addRow(Tr("Settings.Model"), modelRow);
	fallbackModel_ = new QComboBox;
	fallbackModel_->setEditable(true);
	fallbackModel_->setInsertPolicy(QComboBox::NoInsert);
	fallbackModel_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	fallbackModel_->lineEdit()->setPlaceholderText("keins");
	fallbackModel_->setToolTip(Tr("Settings.StepsInWhenMainModel"));
	form->addRow(Tr("Settings.FallbackModel"), fallbackModel_);

	modelStatus_ = hint({});
	form->addRow(modelStatus_);
	connect(loadModels_, &QPushButton::clicked, this, &SettingsDialog::loadModels);

	baseUrl_ = new QLineEdit;
	form->addRow(Tr("Settings.ServerURL"), baseUrl_);

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
	form->addRow(Tr("Settings.Account"), row);
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

	form->addRow(hint(Tr("Settings.ConnectingOpensTwitchInYour")));

	clientId_ = new QLineEdit(qstr(settings_.twitchClientId));
	clientId_->setPlaceholderText(Tr("Settings.EmptyPluginsTwitchApp"));
	form->addRow(Tr("Settings.OwnClientID"), clientId_);
	form->addRow(hint(Tr("Settings.OnlyNeededIfYouWant")));
	twitchTags_ = new QLineEdit(qstr(settings_.twitchTags));
	twitchTags_->setPlaceholderText(Tr("Settings.EGEnglishGamingEmpty"));
	twitchTags_->setToolTip(Tr("Settings.SetOnEveryGameSwitch"));
	form->addRow(Tr("Settings.TagsOnEverySwitch"), twitchTags_);
	return page;
}

QWidget *SettingsDialog::createDetectionTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	enabled_ = new QCheckBox(Tr("Settings.CheckAndSwitchAutomatically"));
	enabled_->setChecked(settings_.enabled);
	form->addRow(enabled_);

	onlyLive_ = new QCheckBox(Tr("Settings.OnlyCheckWhileYoureLive"));
	onlyLive_->setToolTip(Tr("Settings.LiveMeansOBSIsStreaming"));
	onlyLive_->setChecked(settings_.onlyLive);
	form->addRow(onlyLive_);

	interval_ = new QSpinBox;
	interval_->setRange(20, 3600);
	interval_->setSingleStep(10);
	interval_->setSuffix(" s");
	interval_->setValue(settings_.interval);
	form->addRow(Tr("Settings.CheckInterval"), interval_);

	source_ = new QComboBox;
	source_->addItem(Tr("Settings.ProgramOutputWhatViewersSee"), QString());
	for (const std::string &name : videoSources())
		source_->addItem(qstr(name), qstr(name));
	if (!settings_.sourceName.empty() && source_->findData(qstr(settings_.sourceName)) < 0)
		source_->addItem(qstr(settings_.sourceName) + Tr("Settings.NotFound"), qstr(settings_.sourceName));
	source_->setCurrentIndex(std::max(0, source_->findData(qstr(settings_.sourceName))));
	source_->setToolTip(Tr("Settings.SingleSourceLikeYourGame"));
	form->addRow(Tr("Settings.ImageSource"), source_);

	imageWidth_ = new QSpinBox;
	imageWidth_->setRange(512, 1280);
	imageWidth_->setSingleStep(64);
	imageWidth_->setSuffix(" px");
	imageWidth_->setValue(settings_.imageWidth);
	imageWidth_->setToolTip(Tr("Settings.LargerRecognizesDetailsBetterBut"));
	form->addRow(Tr("Settings.ImageWidth"), imageWidth_);

	minConfidence_ = new QSpinBox;
	minConfidence_->setRange(0, 100);
	minConfidence_->setSuffix(" %");
	minConfidence_->setValue(settings_.minConfidence);
	form->addRow(Tr("Settings.MinimumConfidence"), minConfidence_);

	confirmations_ = new QSpinBox;
	confirmations_->setRange(1, 10);
	confirmations_->setValue(settings_.confirmations);
	confirmations_->setToolTip(Tr("Settings.HowManyTimesInRow"));
	form->addRow(Tr("Settings.Confirmations"), confirmations_);

	idleCategory_ = new QLineEdit(qstr(settings_.idleCategory));
	idleCategory_->setPlaceholderText(Tr("Settings.EGJustChattingEmpty"));
	form->addRow(Tr("Settings.NoGame"), idleCategory_);

	windowHint_ = new QCheckBox(Tr("Settings.SendApplicationAndWindowNames"));
	windowHint_->setChecked(settings_.sendWindowHint);
	windowHint_->setToolTip(Tr("Settings.SendsWhichApplicationsOBSCaptures"));
	form->addRow(windowHint_);

	verifyDatabase_ = new QCheckBox(Tr("Settings.CrossCheckWithSteamScreenshots"));
	verifyDatabase_->setChecked(settings_.verifyWithDatabase);
	verifyDatabase_->setToolTip(Tr("Settings.BeforeAISwitchesToAnother"));
	form->addRow(verifyDatabase_);

	blocked_ = new QLineEdit(qstr(settings_.blockedCategories));
	blocked_->setPlaceholderText(Tr("Settings.EGJustChattingBloodborne"));
	blocked_->setToolTip(Tr("Settings.AINeverSwitchesToThese"));
	form->addRow(Tr("Settings.NeverSwitchTo"), blocked_);
	return page;
}

QWidget *SettingsDialog::createTitleTab()
{
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);

	// -- Wie der Titel entsteht
	auto *how = new QGroupBox(Tr("Settings.HowTitleIsMade"));
	auto *form = new QFormLayout(how);
	titleMode_ = new QComboBox;
	titleMode_->addItem(Tr("Settings.LetAIWriteIt"), static_cast<int>(TitleMode::Ai));
	titleMode_->addItem(Tr("Settings.UseTemplate"), static_cast<int>(TitleMode::Template));
	titleMode_->addItem(Tr("Settings.DontChangeTitle"), static_cast<int>(TitleMode::Keep));
	titleMode_->setCurrentIndex(std::max(0, titleMode_->findData(static_cast<int>(settings_.titleMode))));
	form->addRow(Tr("Settings.Title2"), titleMode_);

	titlePreset_ = new QComboBox;
	for (const TitlePreset &preset : titlePresets()) {
		titlePreset_->addItem(Tr(preset.label), QString(preset.id));
		titlePreset_->setItemData(titlePreset_->count() - 1, QString::fromUtf8(preset.style), Qt::ToolTipRole);
	}
	titlePreset_->setCurrentIndex(std::max(0, titlePreset_->findData(qstr(settings_.titlePreset))));
	form->addRow(Tr("Settings.Style"), titlePreset_);

	titleLanguage_ = new QComboBox;
	titleLanguage_->addItem(Tr("Settings.German"), QStringLiteral("de"));
	titleLanguage_->addItem(Tr("Settings.English"), QStringLiteral("en"));
	titleLanguage_->setCurrentIndex(std::max(0, titleLanguage_->findData(qstr(settings_.titleLanguage))));
	titleEmojis_ = new QCheckBox(Tr("Settings.AllowEmojis"));
	titleEmojis_->setChecked(settings_.titleEmojis);
	titleEmojis_->setToolTip(Tr("Settings.OffPluginRemovesEmojisFrom"));
	auto *languageRow = new QHBoxLayout;
	languageRow->addWidget(titleLanguage_);
	languageRow->addWidget(titleEmojis_);
	languageRow->addStretch(1);
	form->addRow(Tr("Settings.Language"), languageRow);

	titleStyle_ = new QPlainTextEdit(qstr(settings_.titleStyle));
	titleStyle_->setMaximumHeight(60);
	titleStyle_->setPlaceholderText(Tr("Settings.OptionalEGMentionThat"));
	form->addRow(Tr("Settings.OwnAdditions"), titleStyle_);
	form->addRow(hint(Tr("Settings.StyleIsReadyMadePreset")));

	titleTemplate_ = new QLineEdit(qstr(settings_.titleTemplate));
	form->addRow(Tr("Settings.Template"), titleTemplate_);
	form->addRow(hint(Tr("Settings.PlaceholdersCodeGameCodeTwitch")));
	outer->addWidget(how);

	// -- Feste Vorgaben
	auto *fixed = new QGroupBox(Tr("Settings.FixedPartsAlwaysApplyWhatever"));
	auto *fixedForm = new QFormLayout(fixed);
	titlePrefix_ = new QLineEdit(qstr(settings_.titlePrefix));
	titlePrefix_->setPlaceholderText(Tr("Settings.EGEN"));
	fixedForm->addRow(Tr("Settings.AlwaysAtStart"), titlePrefix_);
	titleSuffix_ = new QLineEdit(qstr(settings_.titleSuffix));
	titleSuffix_->setPlaceholderText(Tr("Settings.EGDiscordSocials"));
	fixedForm->addRow(Tr("Settings.AlwaysAtEnd"), titleSuffix_);
	fixedForm->addRow(hint(Tr("Settings.PluginAddsThesePartsTo")));
	titlePreview_ = hint(QString());
	titlePreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	fixedForm->addRow(Tr("Settings.Preview"), titlePreview_);
	outer->addWidget(fixed);

	// -- Spielfortschritt
	auto *progress = new QGroupBox(Tr("Settings.GameProgress"));
	auto *progressForm = new QFormLayout(progress);
	progressTitles_ = new QCheckBox(Tr("Settings.AlsoAdjustTitleWhenSomething"));
	progressTitles_->setChecked(settings_.progressTitles);
	progressTitles_->setToolTip(Tr("Settings.NewLevelNewWorldBoss"));
	progressForm->addRow(progressTitles_);
	progressLevel_ = new QComboBox;
	progressLevel_->addItem(Tr("Settings.RarelyOnlyBigSectionsNew"), QStringLiteral("big"));
	progressLevel_->addItem(Tr("Settings.NormalAlsoNewAreasRounds"), QStringLiteral("normal"));
	progressLevel_->addItem(Tr("Settings.OftenOnEveryClearlyDifferent"), QStringLiteral("often"));
	progressLevel_->setCurrentIndex(std::max(0, progressLevel_->findData(qstr(settings_.progressLevel))));
	progressForm->addRow(Tr("Settings.HowOften"), progressLevel_);
	progressMinutes_ = new QSpinBox;
	progressMinutes_->setRange(2, 240);
	progressMinutes_->setSingleStep(1);
	progressMinutes_->setSuffix(" min");
	progressMinutes_->setValue(settings_.progressMinutes);
	progressForm->addRow(Tr("Settings.AtMostEvery"), progressMinutes_);
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
		titlePreview_->setText(Tr("Settings.ITitleStaysAsIt"));
		return;
	}
	const std::string core = mode == TitleMode::Ai
					 ? std::string(Tr("Settings.TitleFromAI"))
					 : fillTemplate(titleTemplate_->text().toStdString(), "Minecraft",
							Tr("Settings.BossFightAgainstEnderDragon"), {"Hardcore"});
	titlePreview_->setText(qstr(composeTitle(example, core)).toHtmlEscaped());
}

QWidget *SettingsDialog::createYouTubeTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);
	form->addRow(hint(Tr("Settings.OnYouTubePluginChangesB")));

	youtubeClientId_ = new QLineEdit(qstr(settings_.youtubeClientId));
	youtubeClientId_->setPlaceholderText("….apps.googleusercontent.com");
	form->addRow(Tr("Settings.ClientID"), youtubeClientId_);
	youtubeClientSecret_ = new QLineEdit(qstr(settings_.youtubeClientSecret));
	youtubeClientSecret_->setEchoMode(QLineEdit::Password);
	form->addRow(Tr("Settings.ClientSecret"), youtubeClientSecret_);

	youtubeStatus_ = new QLabel;
	youtubeButton_ = new QPushButton;
	auto *row = new QHBoxLayout;
	row->addWidget(youtubeStatus_, 1);
	row->addWidget(youtubeButton_);
	form->addRow(Tr("Settings.Account"), row);
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
				QMessageBox::information(this, windowTitle(), Tr("Settings.PleaseEnterClientIDAnd"));
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
						? QString(Tr("Settings.ConfirmCodeInBrowserDots")).arg(s.deviceCode)
						: QString(Tr("Settings.SigningIn")));
		youtubeButton_->setText(Tr("Settings.Cancel"));
	} else if (s.youtubeConnected) {
		youtubeStatus_->setText(Tr("Settings.Connected") + s.youtubeChannel);
		youtubeButton_->setText(Tr("Settings.Disconnect"));
	} else {
		youtubeStatus_->setText(Tr("Settings.NotConnected"));
		youtubeButton_->setText(Tr("Settings.ConnectWithYouTube"));
	}
}

QWidget *SettingsDialog::createVoiceTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	std::string reason;
	const bool supported = voice::supported(&reason);
	voiceEnabled_ = new QCheckBox(Tr("Settings.ListenToYourVoiceAnd"));
	voiceEnabled_->setChecked(settings_.voiceEnabled && supported);
	voiceEnabled_->setEnabled(supported);
	form->addRow(voiceEnabled_);
	form->addRow(hint(supported ? Tr("Settings.SpeechRecognitionRunsEntirelyOn") : "<b>" + qstr(reason) + "</b>"));

	voiceSource_ = new QComboBox;
	voiceSource_->addItem(Tr("Settings.AutomaticFirstMicrophone"), QString());
	for (const voice::AudioSource &source : voice::audioSources()) {
		QString label = qstr(source.name);
		if (!source.device.empty())
			label += " – " + qstr(source.device);
		if (source.global)
			label += Tr("Settings.OBSAudioDevice");
		voiceSource_->addItem(label, qstr(source.name));
	}
	if (!settings_.voiceSource.empty() && voiceSource_->findData(qstr(settings_.voiceSource)) < 0) {
		// Auch andere Audioquellen sind erlaubt (z. B. ein eigener Mikrofon-Filter-Mix)
		obs_source_t *source = obs_get_source_by_name(settings_.voiceSource.c_str());
		voiceSource_->addItem(qstr(settings_.voiceSource) + (source ? "" : Tr("Settings.NotFound")),
				      qstr(settings_.voiceSource));
		obs_source_release(source);
	}
	voiceSource_->setCurrentIndex(std::max(0, voiceSource_->findData(qstr(settings_.voiceSource))));
	form->addRow(Tr("Settings.Microphone"), voiceSource_);

	voiceModel_ = new QComboBox;
	for (const VoiceModelInfo &model : voiceModels())
		voiceModel_->addItem(Tr(model.label), QString(model.id));
	voiceModel_->setCurrentIndex(std::max(0, voiceModel_->findData(qstr(settings_.voiceModel))));
	form->addRow(Tr("Settings.SpeechModel"), voiceModel_);
	form->addRow(hint(Tr("Settings.DownloadedOnceFromHuggingfaceCo")));

	voiceStatus_ = hint(QString());
	form->addRow(Tr("Settings.Status"), voiceStatus_);
	voiceText_ = hint(QString());
	voiceText_->setTextFormat(Qt::PlainText);
	form->addRow(Tr("Settings.LastUnderstood"), voiceText_);

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
	layout->addWidget(hint(Tr("Settings.IfOBSCapturesOneOf")));

	rules_ = new QTableWidget(0, 5);
	rules_->setHorizontalHeaderLabels({Tr("Settings.ApplicationExe"), Tr("Settings.WindowTitleContains"),
					   Tr("Settings.TwitchCategory"), Tr("Settings.OwnTitle"), "Tags"});
	rules_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	rules_->horizontalHeader()->setStretchLastSection(true);
	rules_->setColumnWidth(0, 120);
	rules_->setColumnWidth(1, 130);
	rules_->setColumnWidth(2, 140);
	rules_->setColumnWidth(3, 160);
	rules_->verticalHeader()->setVisible(false);
	rules_->setSelectionBehavior(QAbstractItemView::SelectRows);
	rules_->horizontalHeaderItem(1)->setToolTip(Tr("Settings.EmptyAppliesToEveryWindow"));
	rules_->horizontalHeaderItem(3)->setToolTip(Tr("Settings.EmptyTitleAsSetUnder"));
	rules_->horizontalHeaderItem(4)->setToolTip(Tr("Settings.TwitchTagsForThisGame"));
	for (const GameRule &rule : settings_.rules)
		addRule(rule);
	layout->addWidget(rules_, 1);

	auto *buttons = new QHBoxLayout;
	auto *add = new QPushButton(Tr("Settings.Add"));
	auto *fromCapture = new QPushButton(Tr("Settings.AddCurrentCaptures"));
	fromCapture->setToolTip(Tr("Settings.AddsApplicationsOBSIsCurrently"));
	auto *remove = new QPushButton(Tr("Settings.Remove"));
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

	providerHelp_->setText(Tr(info.help));
	apiKey_->setText(qstr(values.apiKey));
	apiKey_->setEnabled(!isLocalProvider(provider));
	apiKey_->setPlaceholderText(info.needsKey ? "erforderlich" : "optional");

	model_->clear();
	model_->setEditText(qstr(values.model.empty() ? info.defaultModel : values.model));
	fallbackModel_->clear();
	fallbackModel_->setEditText(qstr(values.fallbackModel));
	// Cloudflare: Vorlage eintragen, damit nur noch die Account-ID ersetzt werden muss
	baseUrl_->setText(
		qstr(values.baseUrl.empty() && provider == Provider::Cloudflare ? info.baseUrl : values.baseUrl));
	baseUrl_->setPlaceholderText(*info.baseUrl ? info.baseUrl : Tr("Settings.EGHttpLocalhost1234"));
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
	modelStatus_->setText(Tr("Settings.FetchingModels"));
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
	modelStatus_->setText(models.empty() ? Tr("Settings.ConnectionWorksButProviderReports")
					     : QString(Tr("Settings.ConnectionWorks1ModelsTo")).arg(models.size()));
}

void SettingsDialog::refreshTwitch()
{
	if (!switcher_)
		return;
	const StatusSnapshot s = switcher_->status();
	if (s.loginRunning) {
		twitchStatus_->setText(s.deviceCode.isEmpty() ? Tr("Settings.SigningIn")
							      : Tr("Settings.ConfirmCodeInYourBrowser") + s.deviceCode);
		twitchButton_->setText(Tr("Settings.Cancel"));
	} else if (s.twitchConnected) {
		twitchStatus_->setText(Tr("Settings.ConnectedAs") + s.twitchLogin);
		twitchButton_->setText(Tr("Settings.Disconnect"));
	} else {
		twitchStatus_->setText(Tr("Settings.NotConnected"));
		twitchButton_->setText(Tr("Settings.ConnectWithTwitch"));
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
		QMessageBox::warning(this, windowTitle(), QString(Tr("Settings.1NeedsAPIKey")).arg(Tr(info.label)));
		return;
	}
	if (settings_.activeBaseUrl().empty()) {
		QMessageBox::warning(this, windowTitle(), Tr("Settings.PleaseEnterServerURL"));
		return;
	}

	const std::string uiLanguage = uiLanguage_->currentData().toString().toStdString();
	const bool languageChanged = uiLanguage != settings_.uiLanguage;
	settings_.uiLanguage = uiLanguage;
	switcher_->setSettings(settings_);
	if (languageChanged) {
		i18nInit(uiLanguage);
		QMessageBox::information(this, windowTitle(), Tr("Settings.UiLanguageRestart"));
	}
	accept();
}

} // namespace kgs
