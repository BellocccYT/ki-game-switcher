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

#include <array>
#include <string>
#include <vector>

namespace kgs {

enum class Provider { Gemini = 0, OpenRouter, Ollama, Custom, Claude };
constexpr int kProviderCount = 5;

struct ProviderInfo {
	const char *id; // Schlüssel in der Konfigurationsdatei
	const char *label;
	const char *baseUrl;
	const char *defaultModel;
	const char *defaultFallbackModel; // springt ein, wenn das Hauptmodell überlastet ist
	bool needsKey;
	const char *help; // Rich-Text für den Einstellungsdialog
};

const ProviderInfo &providerInfo(Provider provider);

enum class TitleMode { Ai = 0, Template, Keep };

// Fertige Stilvorgaben für KI-Titel, damit niemand lange Anweisungen schreiben muss
struct TitlePreset {
	const char *id;
	const char *label;
	const char *style; // Anweisung an die KI; leer = nur die eigenen Ergänzungen
};
const std::vector<TitlePreset> &titlePresets();
const TitlePreset &titlePreset(const std::string &id);

// Sprachmodelle für die lokale Spracherkennung (whisper.cpp)
struct VoiceModelInfo {
	const char *id;
	const char *label;
	const char *file; // Dateiname auf huggingface.co/ggerganov/whisper.cpp
	int sizeMb;
};
const std::vector<VoiceModelInfo> &voiceModels();
const VoiceModelInfo &voiceModel(const std::string &id);

// Feste Zuordnung: Nimmt OBS dieses Programm auf, wird ohne KI auf die Kategorie umgestellt
struct GameRule {
	std::string executable;  // z. B. „javaw.exe“
	std::string category;    // z. B. „Minecraft“
	std::string windowTitle; // nur wenn der Fenstertitel das enthält (Emulatoren: mehrere Spiele pro Programm)
	std::string title;       // eigener Titel für dieses Spiel (Platzhalter wie in der Vorlage), leer = normal
	std::string tags;        // zusätzliche Twitch-Tags, durch Komma getrennt
};

// Zerlegt „a, b; c“ bzw. mehrere Zeilen in einzelne, gekürzte Einträge
std::vector<std::string> splitList(const std::string &text);

struct ProviderSettings {
	std::string apiKey;
	std::string model;         // leer = Standardmodell des Anbieters
	std::string fallbackModel; // leer = kein Ersatzmodell
	std::string baseUrl;       // leer = Standard-Server des Anbieters
};

struct Settings {
	Provider provider = Provider::Ollama;
	std::array<ProviderSettings, kProviderCount> providers;
	std::string twitchClientId; // leer = eingebaute Client-ID

	bool enabled = true;
	bool onlyLive = true;
	int interval = 60;      // Sekunden
	std::string sourceName; // leer = Programmausgabe
	int imageWidth = 1024;
	int minConfidence = 70; // Prozent
	int confirmations = 2;
	std::string idleCategory;
	bool sendWindowHint = true;
	bool verifyWithDatabase = true; // vor dem Umstellen mit Steam-Screenshots abgleichen
	std::string blockedCategories;  // darauf stellt die KI nie um (Komma-Liste)
	std::vector<GameRule> rules;

	bool dockIntroduced = false; // Dock beim allerersten Start einmal einblenden

	TitleMode titleMode = TitleMode::Ai;
	std::string titleTemplate = "{game} – live! 🎮";
	std::string titlePreset = "locker";
	std::string titleStyle; // eigene Ergänzungen zur Stilvorgabe
	std::string titlePrefix; // steht immer vorne im Titel, egal welches Modell
	std::string titleSuffix; // steht immer hinten im Titel, egal welches Modell
	std::string titleLanguage = "de"; // „de“ oder „en“
	bool titleEmojis = true;          // aus = das Plugin entfernt Emojis aus Titeln
	std::string twitchTags;           // bei jedem Wechsel setzen (leer = Tags nicht anfassen)
	bool progressTitles = true; // Titel auch bei Fortschritt im selben Spiel anpassen
	int progressMinutes = 15;   // frühestens so oft

	bool voiceEnabled = false;
	std::string voiceSource; // leer = Mikrofon automatisch wählen
	std::string voiceModel = "base";

	const ProviderSettings &activeProvider() const { return providers[static_cast<int>(provider)]; }
	std::string activeModel() const;
	std::string activeBaseUrl() const;
	std::string effectiveTwitchClientId() const;
};

Settings loadSettings();
void saveSettings(const Settings &settings);

// Pfad im Konfigurationsordner des Plugins (…/obs-studio/plugin_config/ki-game-switcher/)
std::string configPath(const char *file);

} // namespace kgs
