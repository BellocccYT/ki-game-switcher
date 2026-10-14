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

#pragma once

#include "settings.hpp"

#include <QDialog>
#include <QPointer>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace kgs {

class Switcher;

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	// Öffnet den Dialog oder holt den bereits offenen nach vorne
	static void open(Switcher *switcher, QWidget *parent);

private:
	SettingsDialog(Switcher *switcher, QWidget *parent);

	QWidget *createAiTab();
	QWidget *createTwitchTab();
	QWidget *createDetectionTab();
	QWidget *createTitleTab();
	QWidget *createVoiceTab();
	QWidget *createYouTubeTab();
	void refreshYouTube();
	QWidget *createRulesTab();
	void updateTitlePreview();
	void refreshVoice();
	void addRule(const GameRule &rule);

	void showProvider(int index);
	void storeProvider(int index);
	void loadModels();
	void setModels(const std::vector<std::string> &models, const QString &error);
	void refreshTwitch();
	void save();

	QPointer<Switcher> switcher_;
	Settings settings_;
	int shownProvider_ = -1;

	QComboBox *provider_;
	QLabel *providerHelp_;
	QLineEdit *apiKey_;
	QCheckBox *showKey_;
	QComboBox *model_;
	QComboBox *fallbackModel_;
	QPushButton *loadModels_;
	QLabel *modelStatus_;
	QLineEdit *baseUrl_;

	QLabel *twitchStatus_;
	QPushButton *twitchButton_;
	QLineEdit *clientId_;

	QCheckBox *enabled_;
	QCheckBox *onlyLive_;
	QSpinBox *interval_;
	QComboBox *source_;
	QSpinBox *imageWidth_;
	QSpinBox *minConfidence_;
	QSpinBox *confirmations_;
	QLineEdit *idleCategory_;
	QCheckBox *windowHint_;
	QCheckBox *verifyDatabase_;
	QLineEdit *blocked_;
	QLineEdit *twitchTags_;

	QComboBox *titleMode_;
	QComboBox *titlePreset_;
	QComboBox *titleLanguage_;
	QCheckBox *titleEmojis_;
	QLineEdit *titleTemplate_;
	QPlainTextEdit *titleStyle_;
	QLineEdit *titlePrefix_;
	QLineEdit *titleSuffix_;
	QLabel *titlePreview_;
	QCheckBox *progressTitles_;
	QComboBox *progressLevel_;
	QSpinBox *progressMinutes_;

	QCheckBox *voiceEnabled_;
	QComboBox *voiceSource_;
	QComboBox *voiceModel_;
	QLabel *voiceStatus_;
	QLabel *voiceText_;

	QTableWidget *rules_;

	QLineEdit *youtubeClientId_;
	QLineEdit *youtubeClientSecret_;
	QLabel *youtubeStatus_;
	QPushButton *youtubeButton_;
};

} // namespace kgs
