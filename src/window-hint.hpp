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

#include <string>

namespace kgs {

struct ForegroundWindow {
	std::string title;
	std::string executable;
};

// Vordergrundfenster; leer, wenn nicht verfügbar (z. B. unter macOS/Linux).
ForegroundWindow foregroundWindow();

// Aktueller Titel des sichtbaren Hauptfensters eines Programms. Emulatoren schreiben dort meist das
// laufende Spiel hinein. Leer, wenn nicht gefunden oder nicht unter Windows.
std::string currentWindowTitle(const std::string &executable);

} // namespace kgs
