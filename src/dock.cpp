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

#include "dock.hpp"

#include "capture-info.hpp"
#include "settings-dialog.hpp"
#include "switcher.hpp"
#include "window-hint.hpp"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>

namespace kgs {

namespace {

constexpr int kMaxHistory = 100;

QString dot(const char *color, const QString &text)
{
	return QString("<span style=\"color:%1\">●</span> %2").arg(color, text.toHtmlEscaped());
}

QLabel *valueLabel()
{
	auto *label = new QLabel("–");
	label->setWordWrap(true);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	return label;
}

} // namespace

SwitcherDock::SwitcherDock(Switcher *switcher, QWidget *parent) : QWidget(parent), switcher_(switcher)
{
	auto *layout = new QVBoxLayout(this);

	state_ = new QLabel;
	state_->setTextFormat(Qt::RichText);
	layout->addWidget(state_);

	preview_ = new QLabel("Noch kein Bild – „Test“ zeigt, was die KI sieht");
	preview_->setAlignment(Qt::AlignCenter);
	preview_->setWordWrap(true);
	// Ignored: Das Vorschaubild nimmt sich den freien Platz, statt das Dock zu vergrößern
	preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
	preview_->setMinimumHeight(60);
	preview_->setStyleSheet("QLabel { background: rgba(0, 0, 0, 60); border-radius: 4px; }");
	preview_->installEventFilter(this);
	layout->addWidget(preview_, 3);

	auto *form = new QFormLayout;
	form_ = form;
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	detection_ = valueLabel();
	category_ = valueLabel();
	title_ = valueLabel();
	scene_ = valueLabel();
	speech_ = valueLabel();
	form->addRow("Erkannt:", detection_);
	form->addRow("Situation:", scene_);
	form->addRow("Kategorie:", category_);
	form->addRow("Titel:", title_);
	form->addRow("Stimme:", speech_);
	layout->addLayout(form);

	auto *twitchRow = new QHBoxLayout;
	twitch_ = new QLabel;
	twitch_->setWordWrap(true);
	twitchButton_ = new QPushButton;
	twitchRow->addWidget(twitch_, 1);
	twitchRow->addWidget(twitchButton_);
	layout->addLayout(twitchRow);

	youtube_ = new QLabel;
	youtube_->setWordWrap(true);
	layout->addWidget(youtube_);

	deviceCode_ = new QLabel;
	deviceCode_->setWordWrap(true);
	deviceCode_->setTextFormat(Qt::RichText);
	deviceCode_->setOpenExternalLinks(true);
	deviceCode_->setStyleSheet("QLabel { border: 1px solid #9146ff; border-radius: 4px; padding: 6px; }");
	layout->addWidget(deviceCode_);

	notice_ = new QLabel;
	notice_->setWordWrap(true);
	notice_->setStyleSheet("QLabel { color: #e3a008; }");
	layout->addWidget(notice_);

	auto *buttons = new QGridLayout;
	checkButton_ = new QPushButton("Jetzt prüfen");
	checkButton_->setToolTip("Prüft sofort und stellt Twitch ohne Bestätigungsrunden um");
	testButton_ = new QPushButton("Test");
	testButton_->setToolTip("Erkennt das Spiel, ändert aber nichts auf Twitch");
	pauseButton_ = new QPushButton;
	settingsButton_ = new QPushButton("Einstellungen …");
	buttons->addWidget(checkButton_, 0, 0);
	buttons->addWidget(testButton_, 0, 1);
	buttons->addWidget(pauseButton_, 1, 0);
	buttons->addWidget(settingsButton_, 1, 1);
	correctButton_ = new QPushButton("Falsch erkannt? Richtiges Spiel setzen …");
	correctButton_->setToolTip("Setzt die Kategorie von Hand und merkt sie sich auf Wunsch für das aufgenommene "
				   "Programm");
	buttons->addWidget(correctButton_, 2, 0, 1, 2);
	layout->addLayout(buttons);

	layout->addWidget(new QLabel("Verlauf:"));
	history_ = new QListWidget;
	history_->setWordWrap(true);
	history_->setMinimumHeight(80);
	layout->addWidget(history_, 2);

	connect(checkButton_, &QPushButton::clicked, this, [this] {
		if (switcher_)
			switcher_->checkNow();
	});
	connect(testButton_, &QPushButton::clicked, this, [this] {
		if (switcher_)
			switcher_->testNow();
	});
	connect(pauseButton_, &QPushButton::clicked, this, [this] {
		if (switcher_)
			switcher_->setEnabled(!switcher_->settings().enabled);
	});
	connect(settingsButton_, &QPushButton::clicked, this, [this] { SettingsDialog::open(switcher_, this); });
	connect(twitchButton_, &QPushButton::clicked, this, &SwitcherDock::onTwitchButton);
	connect(correctButton_, &QPushButton::clicked, this, &SwitcherDock::onCorrect);

	connect(switcher, &Switcher::statusChanged, this, &SwitcherDock::refresh);
	connect(switcher, &Switcher::historyAdded, this, &SwitcherDock::addHistory);
	connect(switcher, &Switcher::previewChanged, this, [this](const QImage &image) {
		previewImage_ = image;
		updatePreview();
	});
	connect(switcher, &Switcher::openUrlRequested, this,
		[](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });

	// Für den Countdown bis zur nächsten Prüfung
	connect(&timer_, &QTimer::timeout, this, &SwitcherDock::refresh);
	timer_.start(1000);
	refresh();
}

void SwitcherDock::refresh()
{
	if (!switcher_)
		return;
	const StatusSnapshot s = switcher_->status();

	if (s.busy)
		state_->setText(dot("#58a6ff", s.busySeconds > 0 ? QString("Prüft gerade … %1 s").arg(s.busySeconds)
								 : QString("Prüft gerade …")));
	else if (!s.enabled)
		state_->setText(dot("#8b949e", "Pausiert"));
	else if (!s.twitchConnected && !s.youtubeConnected)
		state_->setText(dot("#e3a008", "Nicht mit Twitch oder YouTube verbunden"));
	else if (s.onlyLive && !s.streaming)
		state_->setText(dot("#8b949e", "Wartet, bis du live bist (OBS oder Twitch)"));
	else if (s.holdMinutes > 0)
		state_->setText(dot(
			"#3fb950",
			QString("Aktiv – nach deiner Korrektur stellt die KI noch %1 min nicht um").arg(s.holdMinutes)));
	else if (s.secondsUntilCheck >= 0)
		state_->setText(
			dot("#3fb950", QString("Aktiv%1 – nächste Prüfung in %2 s")
					       .arg(s.liveOnTwitch ? " (live laut " + s.liveSource + ")" : QString())
					       .arg(s.secondsUntilCheck)));
	else
		state_->setText(dot("#3fb950", "Aktiv"));

	detection_->setText(s.detection.isEmpty() ? "–" : s.detection);
	detection_->setToolTip(
		s.detectionReason.isEmpty() ? QString() : QString("%1 (%2)").arg(s.detectionReason, s.detectionTime));
	scene_->setText(s.scene.isEmpty() ? "–" : s.scene);
	category_->setText(s.category.isEmpty() ? "–" : s.category);
	// Stimme: zuletzt Verstandenes, sonst der Status (z. B. Download des Sprachmodells)
	form_->setRowVisible(speech_, s.voiceEnabled);
	speech_->setText(s.speech.isEmpty() ? s.voiceStatus : "„" + s.speech + "“");
	speech_->setToolTip(s.voiceStatus);
	title_->setText(s.title.isEmpty() ? "–" : s.title);

	if (s.loginRunning) {
		twitch_->setText("Twitch: Anmeldung läuft …");
		twitchButton_->setText("Abbrechen");
	} else if (s.twitchConnected) {
		twitch_->setText("Twitch: " + s.twitchLogin);
		twitchButton_->setText("Trennen");
	} else {
		twitch_->setText("Twitch: nicht verbunden");
		twitchButton_->setText("Verbinden");
	}

	deviceCode_->setVisible(!s.deviceCode.isEmpty());
	if (!s.deviceCode.isEmpty()) {
		const QString service = s.loginService.isEmpty() ? QStringLiteral("Twitch") : s.loginService;
		deviceCode_->setText(
			QString("Bestätige im Browser den Code <b>%1</b><br><a href=\"%2\">%3-Anmeldung "
				"öffnen</a>")
				.arg(s.deviceCode.toHtmlEscaped(), s.verificationUri.toHtmlEscaped(), service));
	}
	youtube_->setVisible(s.youtubeConnected);
	youtube_->setText("YouTube: " + s.youtubeChannel);

	notice_->setVisible(!s.notice.isEmpty());
	notice_->setText(s.notice);

	pauseButton_->setText(s.enabled ? "Pausieren" : "Fortsetzen");
	checkButton_->setEnabled(!s.busy);
	testButton_->setEnabled(!s.busy);
	correctButton_->setEnabled(s.twitchConnected || s.youtubeConnected);
}

void SwitcherDock::updatePreview()
{
	// Höchstens so hoch wie das Seitenverhältnis verlangt, damit kein leerer Rand entsteht
	const double aspect = previewImage_.isNull() ? 16.0 / 9.0
						     : static_cast<double>(previewImage_.width()) /
							       std::max(1, previewImage_.height());
	preview_->setMaximumHeight(std::max(60, static_cast<int>(preview_->width() / aspect)));
	if (previewImage_.isNull())
		return;

	const qreal ratio = preview_->devicePixelRatioF();
	QPixmap pixmap = QPixmap::fromImage(
		previewImage_.scaled(preview_->size() * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	pixmap.setDevicePixelRatio(ratio);
	preview_->setPixmap(pixmap);
}

bool SwitcherDock::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == preview_ && event->type() == QEvent::Resize)
		updatePreview();
	return QWidget::eventFilter(watched, event);
}

void SwitcherDock::addHistory(const QString &entry)
{
	history_->insertItem(0, entry);
	while (history_->count() > kMaxHistory)
		delete history_->takeItem(history_->count() - 1);
}

void SwitcherDock::onTwitchButton()
{
	if (!switcher_)
		return;
	const StatusSnapshot s = switcher_->status();
	if (s.loginRunning) {
		switcher_->cancelTwitchLogin();
	} else if (s.twitchConnected) {
		if (QMessageBox::question(this, "TitleSync",
					  "Twitch-Verbindung wirklich trennen? Titel und Kategorie werden dann "
					  "nicht mehr automatisch umgestellt.") == QMessageBox::Yes)
			switcher_->disconnectTwitch();
	} else {
		switcher_->connectTwitch();
	}
}

void SwitcherDock::onCorrect()
{
	if (!switcher_)
		return;

	// Nicht blockierend (open statt exec), damit OBS weiterläuft, solange der Dialog offen ist
	auto *dialog = new QDialog(this);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle("Richtiges Spiel setzen");
	dialog->setMinimumWidth(420);
	auto *form = new QFormLayout(dialog);

	auto *category = new QLineEdit;
	category->setPlaceholderText("Twitch-Kategorie, z. B. Minecraft");
	form->addRow("Spiel:", category);

	// Programme, für die man sich die Kategorie merken kann
	auto *remember = new QComboBox;
	remember->addItem("nicht merken", QString());
	std::set<QString> seen;
	int preselect = 0;
	for (const CapturedApp &app : activeCapturedApps()) {
		const QString exe = QString::fromStdString(app.executable);
		if (!seen.insert(exe.toLower()).second)
			continue;
		remember->addItem(QString("%1 (%2)").arg(exe, app.gameCapture ? "Spielaufnahme" : "Fensteraufnahme"),
				  exe);
		if (app.gameCapture && preselect == 0)
			preselect = remember->count() - 1;
	}
	const QString foreground = QString::fromStdString(foregroundWindow().executable);
	if (!foreground.isEmpty() && !foreground.startsWith("obs", Qt::CaseInsensitive) &&
	    seen.insert(foreground.toLower()).second)
		remember->addItem(foreground + " (aktives Fenster)", foreground);
	remember->setCurrentIndex(preselect);
	form->addRow("Merken für:", remember);

	// Emulatoren: nur für dieses Spiel merken (Teil des Fenstertitels), andere Spiele bleiben unberührt
	std::map<QString, QString> emulatorGames;
	for (const CapturedApp &app : activeCapturedApps()) {
		if (isEmulator(app.executable))
			emulatorGames[QString::fromStdString(app.executable)] =
				QString::fromStdString(gameFromWindowTitle(app.title));
	}
	auto *windowTitle = new QLineEdit;
	windowTitle->setPlaceholderText("Teil des Fenstertitels, z. B. Mario Kart");
	form->addRow("Nur wenn Fenstertitel enthält:", windowTitle);
	auto updateWindowTitle = [form, remember, windowTitle, emulatorGames, category] {
		const auto it = emulatorGames.find(remember->currentData().toString());
		const bool emulator = it != emulatorGames.end();
		form->setRowVisible(windowTitle, emulator);
		windowTitle->setText(emulator ? it->second : QString());
		if (emulator && category->text().trimmed().isEmpty())
			category->setText(it->second);
	};
	connect(remember, &QComboBox::currentIndexChanged, dialog, updateWindowTitle);
	updateWindowTitle();

	auto *info =
		new QLabel("Mit „Merken“ stellt das Plugin künftig sofort und ohne KI auf dieses Spiel um, "
			   "sobald OBS das Programm aufnimmt – bei Emulatoren nur, wenn der Fenstertitel passt. Ohne "
			   "Merken stellt die KI 15 Minuten lang nicht um.");
	info->setWordWrap(true);
	form->addRow(info);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setText("Setzen");
	buttons->button(QDialogButtonBox::Cancel)->setText("Abbrechen");
	connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	form->addRow(buttons);

	connect(dialog, &QDialog::accepted, this, [this, category, remember, windowTitle] {
		if (switcher_ && !category->text().trimmed().isEmpty())
			switcher_->correctCategory(category->text().trimmed().toStdString(),
						   remember->currentData().toString().toStdString(),
						   windowTitle->text().trimmed().toStdString());
	});
	dialog->open();
}

} // namespace kgs
