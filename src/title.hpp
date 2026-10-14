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

#include "settings.hpp"

#include <string>
#include <vector>

namespace kgs {

constexpr int kTwitchTitleLimit = 140;

// Platz, der nach den festen Vorgaben (vorne/hinten) für den eigentlichen Titel bleibt
int coreTitleLimit(const Settings &settings);

// Setzt die festen Vorgaben vorne und hinten an – unabhängig vom Modell – und kürzt auf 140 Zeichen.
// Steht eine Vorgabe schon im Kern (die KI hat sie übernommen), wird sie nicht doppelt angehängt.
std::string composeTitle(const Settings &settings, const std::string &core, int limit = kTwitchTitleLimit);

// Füllt die Vorlage: {game}, {scene}, {keywords}. Leere Platzhalter verschwinden samt Trennzeichen.
std::string fillTemplate(const std::string &templ, const std::string &game, const std::string &scene,
			 const std::vector<std::string> &keywords);

// Entfernt Emojis (für „Emojis aus“ – unabhängig davon, was das Modell liefert)
std::string stripEmojis(const std::string &text);

// Sprache der Titel für die KI („German“ / „English“)
std::string titleLanguageName(const Settings &settings);

// Twitch-Tags: nur Buchstaben und Ziffern, höchstens 25 Zeichen, höchstens 10 Stück, ohne Doppelte
std::vector<std::string> cleanTags(const std::vector<std::string> &tags);

// Stilvorgabe plus eigene Ergänzungen als Anweisung an die KI
std::string titleStyleFor(const Settings &settings);

} // namespace kgs
