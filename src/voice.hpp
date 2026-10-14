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

#include <string>
#include <vector>

namespace kgs {

// Lokale Spracherkennung: hört bei einer OBS-Audioquelle (Mikrofon) mit und wandelt das Gesprochene
// mit whisper.cpp in Text um. Nichts davon verlässt den PC. Alle Methoden sind threadsicher.
namespace voice {

// Prüft, ob die Spracherkennung auf diesem System läuft; reason erklärt sonst, warum nicht
bool supported(std::string *reason = nullptr);

// Übernimmt die Einstellungen: hängt sich an die Audioquelle, lädt bei Bedarf das Sprachmodell herunter
// oder gibt alles frei, wenn die Spracherkennung aus ist.
void configure(const Settings &settings);

// Wandelt das seit dem letzten Aufruf Gesprochene in Text um (blockiert einige Sekunden) und liefert,
// was in den letzten Minuten gesagt wurde. hint (z. B. der Spielname) hilft beim Erkennen von Namen.
std::string listen(const std::string &hint);

std::string status();   // z. B. „Hört zu: Mic/Aux“, „Sprachmodell wird geladen … 40 %“
std::string lastText(); // zuletzt erkannter Satz für die Anzeige

struct AudioSource {
	std::string name;    // Name der Quelle in OBS
	std::string device;  // Gerätename, z. B. „Mikrofon (beyerdynamic MMX 150)“; leer, wenn unbekannt
	bool global = false; // Audiogerät aus den OBS-Einstellungen (z. B. Mic/Aux)
};

// Audioquellen, die als Mikrofon infrage kommen – die wahrscheinlichste zuerst. Globales Mic/Aux geht vor,
// Ton von Capture-Cards (Elgato, HDMI …) landet hinten.
std::vector<AudioSource> audioSources();

// Beim Beenden von OBS: Audioquelle loslassen, Downloads beenden, Modell freigeben
void shutdown();

} // namespace voice

} // namespace kgs
