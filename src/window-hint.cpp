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

#include "window-hint.hpp"

#ifdef _WIN32
#include <QFileInfo>
#include <QString>

#include <iterator>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace kgs {

#ifdef _WIN32

ForegroundWindow foregroundWindow()
{
	ForegroundWindow result;
	HWND hwnd = GetForegroundWindow();
	if (!hwnd)
		return result;

	wchar_t title[512] = {};
	GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
	result.title = QString::fromWCharArray(title).toStdString();

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
		wchar_t path[MAX_PATH * 2] = {};
		DWORD size = static_cast<DWORD>(std::size(path));
		if (QueryFullProcessImageNameW(process, 0, path, &size))
			result.executable = QFileInfo(QString::fromWCharArray(path, static_cast<int>(size)))
						    .fileName()
						    .toStdString();
		CloseHandle(process);
	}
	return result;
}

namespace {

QString processName(HWND hwnd)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	QString name;
	if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
		wchar_t path[MAX_PATH * 2] = {};
		DWORD size = static_cast<DWORD>(std::size(path));
		if (QueryFullProcessImageNameW(process, 0, path, &size))
			name = QFileInfo(QString::fromWCharArray(path, static_cast<int>(size))).fileName();
		CloseHandle(process);
	}
	return name;
}

struct TitleSearch {
	QString executable;
	QString title;
};

BOOL CALLBACK findTitle(HWND hwnd, LPARAM param)
{
	auto *search = reinterpret_cast<TitleSearch *>(param);
	// Nur sichtbare Hauptfenster mit Titel – keine Tool- oder Hilfsfenster
	if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
	    (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
		return TRUE;
	wchar_t title[512] = {};
	if (GetWindowTextW(hwnd, title, static_cast<int>(std::size(title))) == 0)
		return TRUE;
	if (processName(hwnd).compare(search->executable, Qt::CaseInsensitive) != 0)
		return TRUE;
	search->title = QString::fromWCharArray(title);
	return FALSE;
}

} // namespace

std::string currentWindowTitle(const std::string &executable)
{
	TitleSearch search{QString::fromStdString(executable), {}};
	EnumWindows(findTitle, reinterpret_cast<LPARAM>(&search));
	return search.title.toStdString();
}

#else

ForegroundWindow foregroundWindow()
{
	return {};
}

std::string currentWindowTitle(const std::string &)
{
	return {};
}

#endif

} // namespace kgs
