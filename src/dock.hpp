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

#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QFormLayout;
class QLabel;
class QListWidget;
class QPushButton;

namespace kgs {

class Switcher;

// Das Dock-Fenster in OBS: Status, Vorschau, letzte Erkennung, Steuerknöpfe und Verlauf.
class SwitcherDock : public QWidget {
	Q_OBJECT

public:
	explicit SwitcherDock(Switcher *switcher, QWidget *parent = nullptr);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void refresh();
	void updatePreview();
	void addHistory(const QString &entry);
	void onTwitchButton();
	void onCorrect();

	QPointer<Switcher> switcher_;
	QFormLayout *form_;
	QLabel *state_;
	QLabel *preview_;
	QLabel *detection_;
	QLabel *scene_;
	QLabel *speech_;
	QLabel *category_;
	QLabel *title_;
	QLabel *twitch_;
	QLabel *youtube_;
	QLabel *deviceCode_;
	QLabel *notice_;
	QPushButton *twitchButton_;
	QPushButton *checkButton_;
	QPushButton *testButton_;
	QPushButton *pauseButton_;
	QPushButton *settingsButton_;
	QPushButton *correctButton_;
	QListWidget *history_;
	QImage previewImage_;
	QTimer timer_;
};

} // namespace kgs
