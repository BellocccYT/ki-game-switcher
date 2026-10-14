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
#include <vector>

namespace kgs {

struct CapturedApp {
	std::string executable; // z. B. „eldenring.exe“
	std::string title;      // Fenstertitel
	bool gameCapture;       // Spielaufnahme (sonst Fensteraufnahme)
};

// Verfolgt, welche Programme die Spielaufnahmen gerade erfassen (Signal „hooked“ der win-capture-Quellen).
void captureInfoStart();
void captureInfoStop();

// Programme, die Spiel- oder Fensteraufnahmen gerade in der Programmausgabe zeigen. Thread-sicher.
std::vector<CapturedApp> activeCapturedApps();

// Bekannte Emulatoren – ein Programm, viele Spiele; unterschieden wird am Fenstertitel
bool isEmulator(const std::string &executable);

// Rät aus dem Fenstertitel eines Emulators den Spielnamen, z. B.
// „Dolphin 2412 | JIT64 | Direct3D 11 | Super Mario Sunshine (GMSE01)“ → „Super Mario Sunshine“
std::string gameFromWindowTitle(const std::string &title);

} // namespace kgs
