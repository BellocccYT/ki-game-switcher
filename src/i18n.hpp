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

#include <initializer_list>
#include <string>

namespace kgs {

// Oberflächensprache: "auto" folgt der Sprache von OBS, sonst "de" oder "en".
// Vor dem Aufbau der Oberfläche aufrufen; ein späterer Aufruf gilt für alles, was danach entsteht.
void i18nInit(const std::string &language);
void i18nFree();

// Text zum Schlüssel aus data/locale/*.ini (fehlt er, kommt Englisch, notfalls der Schlüssel selbst).
const char *Tr(const char *key);

// Wie Tr, ersetzt aber %1, %2 … durch die Argumente.
std::string Trf(const char *key, std::initializer_list<std::string> args);

} // namespace kgs
