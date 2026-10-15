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
#include "i18n.hpp"
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

	preview_ = new QLabel(Tr("Dock.NoImageYetTestShows"));
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
	form->addRow(Tr("Dock.Detected"), detection_);
	form->addRow(Tr("Dock.Situation"), scene_);
	form->addRow(Tr("Dock.Category"), category_);
	form->addRow(Tr("Dock.Title"), title_);
	form->addRow(Tr("Dock.Voice"), speech_);
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
	checkButton_ = new QPushButton(Tr("Dock.CheckNow"));
	checkButton_->setToolTip(Tr("Dock.ChecksImmediatelyAndSwitchesTwitch"));
	testButton_ = new QPushButton("Test");
	testButton_->setToolTip(Tr("Dock.DetectsGameButChangesNothing"));
	pauseButton_ = new QPushButton;
	settingsButton_ = new QPushButton(Tr("Dock.Settings"));
	buttons->addWidget(checkButton_, 0, 0);
	buttons->addWidget(testButton_, 0, 1);
	buttons->addWidget(pauseButton_, 1, 0);
	buttons->addWidget(settingsButton_, 1, 1);
	correctButton_ = new QPushButton(Tr("Dock.WrongGameSetRightOne"));
	correctButton_->setToolTip(Tr("Dock.SetsCategoryManuallyAndOptionally"));
	buttons->addWidget(correctButton_, 2, 0, 1, 2);
	layout->addLayout(buttons);

	layout->addWidget(new QLabel(Tr("Dock.History")));
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
		state_->setText(dot("#58a6ff", s.busySeconds > 0 ? QString(Tr("Dock.Checking1S")).arg(s.busySeconds)
								 : QString(Tr("Dock.Checking"))));
	else if (!s.enabled)
		state_->setText(dot("#8b949e", Tr("Dock.Paused")));
	else if (!s.twitchConnected && !s.youtubeConnected)
		state_->setText(dot("#e3a008", Tr("Dock.NotConnectedToTwitchOr")));
	else if (s.onlyLive && !s.streaming)
		state_->setText(dot("#8b949e", Tr("Dock.WaitingUntilYoureLiveOBS")));
	else if (s.holdMinutes > 0)
		state_->setText(dot("#3fb950", QString(Tr("Dock.ActiveAfterYourCorrectionAI")).arg(s.holdMinutes)));
	else if (s.secondsUntilCheck >= 0)
		state_->setText(dot("#3fb950",
				    QString(Tr("Dock.Active1NextCheckIn"))
					    .arg(s.liveOnTwitch ? QString(Tr("Dock.LiveAccordingTo")).arg(s.liveSource)
								: QString())
					    .arg(s.secondsUntilCheck)));
	else
		state_->setText(dot("#3fb950", Tr("Dock.Active")));

	detection_->setText(s.detection.isEmpty() ? "–" : s.detection);
	detection_->setToolTip(
		s.detectionReason.isEmpty() ? QString() : QString("%1 (%2)").arg(s.detectionReason, s.detectionTime));
	scene_->setText(s.scene.isEmpty() ? "–" : s.scene);
	category_->setText(s.category.isEmpty() ? "–" : s.category);
	// Stimme: zuletzt Verstandenes, sonst der Status (z. B. Download des Sprachmodells)
	form_->setRowVisible(speech_, s.voiceEnabled);
	speech_->setText(s.speech.isEmpty() ? s.voiceStatus : QString(Tr("Dock.QuotedSpeech")).arg(s.speech));
	speech_->setToolTip(s.voiceStatus);
	title_->setText(s.title.isEmpty() ? "–" : s.title);

	if (s.loginRunning) {
		twitch_->setText(Tr("Dock.TwitchSigningIn"));
		twitchButton_->setText(Tr("Dock.Cancel"));
	} else if (s.twitchConnected) {
		twitch_->setText("Twitch: " + s.twitchLogin);
		twitchButton_->setText(Tr("Dock.Disconnect"));
	} else {
		twitch_->setText(Tr("Dock.TwitchNotConnected"));
		twitchButton_->setText(Tr("Dock.Connect"));
	}

	deviceCode_->setVisible(!s.deviceCode.isEmpty());
	if (!s.deviceCode.isEmpty()) {
		const QString service = s.loginService.isEmpty() ? QStringLiteral("Twitch") : s.loginService;
		deviceCode_->setText(
			QString(Tr("Dock.ConfirmCodeB1B"))
				.arg(s.deviceCode.toHtmlEscaped(), s.verificationUri.toHtmlEscaped(), service));
	}
	youtube_->setVisible(s.youtubeConnected);
	youtube_->setText("YouTube: " + s.youtubeChannel);

	notice_->setVisible(!s.notice.isEmpty());
	notice_->setText(s.notice);

	pauseButton_->setText(s.enabled ? Tr("Dock.Pause") : Tr("Dock.Resume"));
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
		if (QMessageBox::question(this, "TitleSync", Tr("Dock.ReallyDisconnectTwitchTitleAnd")) ==
		    QMessageBox::Yes)
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
	dialog->setWindowTitle(Tr("Dock.SetRightGame"));
	dialog->setMinimumWidth(420);
	auto *form = new QFormLayout(dialog);

	auto *category = new QLineEdit;
	category->setPlaceholderText(Tr("Dock.TwitchCategoryEGMinecraft"));
	form->addRow(Tr("Dock.Game"), category);

	// Programme, für die man sich die Kategorie merken kann
	auto *remember = new QComboBox;
	remember->addItem(Tr("Dock.DontRemember"), QString());
	std::set<QString> seen;
	int preselect = 0;
	for (const CapturedApp &app : activeCapturedApps()) {
		const QString exe = QString::fromStdString(app.executable);
		if (!seen.insert(exe.toLower()).second)
			continue;
		remember->addItem(QString("%1 (%2)").arg(exe, app.gameCapture ? Tr("Dock.GameCapture")
									      : Tr("Dock.WindowCapture")),
				  exe);
		if (app.gameCapture && preselect == 0)
			preselect = remember->count() - 1;
	}
	const QString foreground = QString::fromStdString(foregroundWindow().executable);
	if (!foreground.isEmpty() && !foreground.startsWith("obs", Qt::CaseInsensitive) &&
	    seen.insert(foreground.toLower()).second)
		remember->addItem(foreground + Tr("Dock.ActiveWindow"), foreground);
	remember->setCurrentIndex(preselect);
	form->addRow(Tr("Dock.RememberFor"), remember);

	// Emulatoren: nur für dieses Spiel merken (Teil des Fenstertitels), andere Spiele bleiben unberührt
	std::map<QString, QString> emulatorGames;
	for (const CapturedApp &app : activeCapturedApps()) {
		if (isEmulator(app.executable))
			emulatorGames[QString::fromStdString(app.executable)] =
				QString::fromStdString(gameFromWindowTitle(app.title));
	}
	auto *windowTitle = new QLineEdit;
	windowTitle->setPlaceholderText(Tr("Dock.PartOfWindowTitleE"));
	form->addRow(Tr("Dock.OnlyIfWindowTitleContains"), windowTitle);
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

	auto *info = new QLabel(Tr("Dock.WithRememberPluginSwitchesTo"));
	info->setWordWrap(true);
	form->addRow(info);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setText(Tr("Dock.Set"));
	buttons->button(QDialogButtonBox::Cancel)->setText(Tr("Dock.Cancel"));
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
