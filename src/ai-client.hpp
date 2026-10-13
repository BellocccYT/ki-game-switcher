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

#include "frame-grabber.hpp"
#include "settings.hpp"

#include <string>
#include <vector>

namespace kgs {

struct AiTarget {
	Provider provider = Provider::Gemini;
	std::string baseUrl;
	std::string apiKey;
	std::string model;
	std::string fallbackModel; // leer = kein Ersatzmodell
};

AiTarget aiTargetFrom(const Settings &settings);

// Zusatzwissen für die Erkennung
struct DetectContext {
	std::string appHint;       // aufgenommene Programme und aktives Fenster
	std::string previousGame;  // zuletzt erkanntes Spiel (Twitch-Kategorie)
	std::string previousScene; // Spielsituation beim letzten Titel
	std::string language = "German"; // Sprache für Situation und Stichwörter
};

struct Detection {
	bool isGame = false;
	std::string game;
	std::string category; // Name der Twitch-Kategorie laut KI
	double confidence = 0.0;
	std::string reason;
	std::string scene;           // kurze Beschreibung der Spielsituation, z. B. „Bosskampf gegen …“
	bool progressChanged = false; // deutlich neuer Abschnitt im selben Spiel
	std::vector<std::string> alternatives; // andere infrage kommende Spiele
	std::vector<std::string> keywords;     // Stichwörter aus dem Gesprochenen (extractKeywords)
	std::string model; // Modell, das tatsächlich geantwortet hat
};

struct Verification {
	bool sameGame = false;
	double confidence = 0.0;
	std::string reason;
	std::string model;
};

struct TitleRequest {
	std::string game;
	std::string scene;
	std::vector<std::string> keywords;
	std::string style;
	std::string previousTitle;
	std::string fixedParts; // feste Vorgaben, die der Code selbst anhängt – die KI soll sie nicht wiederholen
	int maxLength = 140;
	std::string language = "German";
	bool emojis = true;
};

// Alle Funktionen blockieren und werfen std::runtime_error mit einer lesbaren deutschen Meldung.
Detection detectGame(const AiTarget &target, const EncodedImage &image, const DetectContext &context);
// Prüft, ob das Streambild dasselbe Spiel zeigt wie die Referenz-Screenshots (nebeneinander in einem Bild)
Verification verifyGame(const AiTarget &target, const EncodedImage &frame, const EncodedImage &references,
			const std::string &candidate);
std::string writeTitle(const AiTarget &target, const TitleRequest &request);
// Zieht aus dem Gesprochenen bis zu drei Stichwörter für den Titel (reine Textanfrage, schnell)
std::vector<std::string> extractKeywords(const AiTarget &target, const std::string &speech, const std::string &game,
					 const std::string &language);
std::vector<std::string> listModels(const AiTarget &target);

} // namespace kgs
