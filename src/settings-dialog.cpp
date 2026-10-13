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

#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
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
	setMinimumWidth(520);

	auto *tabs = new QTabWidget;
	tabs->addTab(createAiTab(), "KI");
	tabs->addTab(createTwitchTab(), "Twitch");
	tabs->addTab(createDetectionTab(), "Erkennung");
	tabs->addTab(createTitleTab(), "Titel");
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
	refreshTwitch();
}

QWidget *SettingsDialog::createAiTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	provider_ = new QComboBox;
	for (int i = 0; i < kProviderCount; i++)
		provider_->addItem(providerInfo(static_cast<Provider>(i)).label);
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

	connect(provider_, &QComboBox::currentIndexChanged, this, [this](int index) {
		storeProvider(shownProvider_);
		showProvider(index);
	});
	provider_->setCurrentIndex(static_cast<int>(settings_.provider));
	showProvider(provider_->currentIndex());
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
	return page;
}

QWidget *SettingsDialog::createDetectionTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	enabled_ = new QCheckBox("Automatisch prüfen und umstellen");
	enabled_->setChecked(settings_.enabled);
	form->addRow(enabled_);

	onlyLive_ = new QCheckBox("Nur prüfen, während der Stream läuft");
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
	return page;
}

QWidget *SettingsDialog::createTitleTab()
{
	auto *page = new QWidget;
	auto *form = formPage(page);

	titleMode_ = new QComboBox;
	titleMode_->addItem("Von der KI schreiben lassen", static_cast<int>(TitleMode::Ai));
	titleMode_->addItem("Vorlage verwenden", static_cast<int>(TitleMode::Template));
	titleMode_->addItem("Titel nicht ändern", static_cast<int>(TitleMode::Keep));
	titleMode_->setCurrentIndex(std::max(0, titleMode_->findData(static_cast<int>(settings_.titleMode))));
	form->addRow("Titel:", titleMode_);

	titleTemplate_ = new QLineEdit(qstr(settings_.titleTemplate));
	form->addRow("Vorlage:", titleTemplate_);
	form->addRow(hint("<code>{game}</code> wird durch die Kategorie ersetzt. Die Vorlage wird auch genutzt, "
			  "wenn der KI-Titel fehlschlägt."));

	titleStyle_ = new QPlainTextEdit(qstr(settings_.titleStyle));
	titleStyle_->setMaximumHeight(90);
	form->addRow("Stil für KI-Titel:", titleStyle_);
	form->addRow(hint("Freitext, z. B. „Kurz und witzig, immer mit ‚!discord‘ am Ende“."));
	return page;
}

QWidget *SettingsDialog::createRulesTab()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout(page);
	layout->addWidget(hint("Nimmt OBS eines dieser Programme auf (Spiel- oder Fensteraufnahme) oder ist es das "
			       "aktive Fenster, stellt das Plugin sofort und ohne KI auf die Kategorie um. Ideal für "
			       "PC-Spiele – 100 % sicher und kostenlos."));

	rules_ = new QTableWidget(0, 2);
	rules_->setHorizontalHeaderLabels({"Programm (.exe)", "Twitch-Kategorie"});
	rules_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	rules_->verticalHeader()->setVisible(false);
	rules_->setSelectionBehavior(QAbstractItemView::SelectRows);
	for (const GameRule &rule : settings_.rules)
		addRule(qstr(rule.executable), qstr(rule.category));
	layout->addWidget(rules_, 1);

	auto *buttons = new QHBoxLayout;
	auto *add = new QPushButton("Hinzufügen");
	auto *fromCapture = new QPushButton("Aktuelle Aufnahmen übernehmen");
	fromCapture->setToolTip("Trägt die Programme ein, die OBS gerade aufnimmt – du ergänzt nur noch die Kategorie");
	auto *remove = new QPushButton("Entfernen");
	buttons->addWidget(add);
	buttons->addWidget(fromCapture);
	buttons->addStretch(1);
	buttons->addWidget(remove);
	layout->addLayout(buttons);

	connect(add, &QPushButton::clicked, this, [this] {
		addRule({}, {});
		rules_->editItem(rules_->item(rules_->rowCount() - 1, 0));
	});
	connect(fromCapture, &QPushButton::clicked, this, [this] {
		for (const CapturedApp &app : activeCapturedApps()) {
			const QString exe = qstr(app.executable);
			if (rules_->findItems(exe, Qt::MatchFixedString).isEmpty())
				addRule(exe, {});
		}
	});
	connect(remove, &QPushButton::clicked, this, [this] {
		const int row = rules_->currentRow();
		if (row >= 0)
			rules_->removeRow(row);
	});
	return page;
}

void SettingsDialog::addRule(const QString &executable, const QString &category)
{
	const int row = rules_->rowCount();
	rules_->insertRow(row);
	rules_->setItem(row, 0, new QTableWidgetItem(executable));
	rules_->setItem(row, 1, new QTableWidgetItem(category));
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
	apiKey_->setEnabled(provider != Provider::Ollama);
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

	settings_.titleMode = static_cast<TitleMode>(titleMode_->currentData().toInt());
	settings_.titleTemplate = titleTemplate_->text().toStdString();
	settings_.titleStyle = titleStyle_->toPlainText().trimmed().toStdString();

	settings_.rules.clear();
	for (int row = 0; row < rules_->rowCount(); row++) {
		const QString executable = rules_->item(row, 0) ? rules_->item(row, 0)->text().trimmed() : QString();
		const QString category = rules_->item(row, 1) ? rules_->item(row, 1)->text().trimmed() : QString();
		if (!executable.isEmpty() && !category.isEmpty())
			settings_.rules.push_back({executable.toStdString(), category.toStdString()});
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
