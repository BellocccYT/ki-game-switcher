# KI Game Switcher für OBS

Ein OBS-Plugin, das per KI erkennt, welches Spiel gerade in deinem Stream läuft, und bei einem Spielwechsel **Twitch-Kategorie und -Titel automatisch umstellt**. Es läuft komplett in OBS und bringt ein eigenes Dock zur Steuerung mit.

- **Kostenlos nutzbar:** Mit Google Gemini (kostenloser API-Key), kostenlosen OpenRouter-Modellen oder ganz lokal mit Ollama
- **Erkennt am echten Streambild:** Gerendert wird direkt in OBS, also genau das, was deine Zuschauer sehen. Alternativ nur die Spielaufnahme, dann klappt es auch mit Konsole oder Capture-Card.
- **Kein nervöses Hin- und Herspringen:** Gewechselt wird erst nach mehreren sicheren Erkennungen hintereinander. Alt-Tab zu Discord löst nichts aus.
- **Titel nach deinem Geschmack:** Die KI schreibt ihn in deinem Stil, oder du nimmst eine Vorlage wie `{game} – live! 🎮`
- **Dock in OBS:** Live-Status, Vorschaubild, letzte Erkennung, „Jetzt prüfen“, „Test“, Pause und Verlauf

## Installation (Windows)

1. Unter **Releases** die Datei `ki-game-switcher-…-windows-x64.zip` herunterladen
2. OBS schließen
3. Den Inhalt der ZIP nach `C:\ProgramData\obs-studio\plugins\` entpacken. Danach existiert `C:\ProgramData\obs-studio\plugins\ki-game-switcher\bin\64bit\ki-game-switcher.dll`.
4. OBS starten → **Docks → KI Game Switcher**

Voraussetzung ist OBS Studio 31 oder neuer.

> **„Fehler beim Laden von Plugins“?** Ist unter Windows 11 die *Intelligente App-Steuerung* (Smart App Control) aktiv, blockiert sie unsignierte DLLs. Im OBS-Log steht dann `LoadLibrary failed … (4551)`. Abhilfe schaffen nur eine signierte Version des Plugins oder das Abschalten der Intelligenten App-Steuerung (Windows-Sicherheit → App- & Browsersteuerung).

## Einrichtung

Öffne im Dock **Einstellungen …** (oder **Werkzeuge → KI Game Switcher – Einstellungen**).

### 1. KI auswählen

| Anbieter | Kosten | Was du brauchst |
|---|---|---|
| **Google Gemini** (Standard) | kostenlos im Rahmen des Gratis-Kontingents | Kostenlosen API-Key von [aistudio.google.com/apikey](https://aistudio.google.com/apikey) |
| **OpenRouter** | kostenlose Modelle | Key von [openrouter.ai/keys](https://openrouter.ai/keys). Ohne Guthaben nur ca. 50 Anfragen/Tag, Intervall daher ≥ 5 Min. |
| **Ollama** | kostenlos, läuft lokal, ohne Key und ohne Limits | [Ollama](https://ollama.com) installieren und `ollama pull qwen3-vl:4b-instruct` (3,3 GB). Braucht ca. 4 GB Grafikspeicher und ein paar Sekunden GPU-Zeit pro Prüfung. AMD-Karten laufen über Vulkan (getestet mit RX 6750 XT: ca. 6–7 s pro Prüfung). |
| **Eigener Server** | – | Jede OpenAI-kompatible Schnittstelle, z. B. LM Studio |
| **Anthropic Claude** | kostenpflichtig | Key von [platform.claude.com](https://platform.claude.com) |

Key eintragen und auf **Modelle laden** klicken. Damit testest du gleich, ob der Key funktioniert, und kannst ein Modell auswählen.

Ist das Hauptmodell überlastet (HTTP 503), sein Gratis-Limit erreicht (HTTP 429) oder antwortet es nicht, springt automatisch das **Ersatzmodell** ein. Bei Gemini ist das standardmäßig `gemini-3.5-flash-lite`, änderbar in den Einstellungen.

### 2. Twitch verbinden

Reiter **Twitch → Mit Twitch verbinden**. Im Browser öffnet sich Twitch. Dort bestätigst du den angezeigten Code, fertig. Das Plugin bekommt dabei nur das Recht, Titel und Kategorie deines Kanals zu ändern (`channel:manage:broadcast`).

### 3. Ausprobieren

Starte ein Spiel und klicke im Dock auf **Test**. Du siehst dann das Bild, das die KI bekommen hat, und was sie erkannt hat. Auf Twitch wird dabei nichts geändert.

Standardmäßig prüft das Plugin **nur während des Streams** jede Minute. Beim Go-Live wird sofort geprüft.

## Genauer erkennen

- **Feste Zuordnungen (100 % sicher):** Im Reiter **Zuordnungen** legst du fest, welches Programm zu welcher Twitch-Kategorie gehört, z. B. `javaw.exe → Minecraft`. Nimmt OBS das Programm auf (Spiel- oder Fensteraufnahme) oder ist es das aktive Fenster, stellt das Plugin sofort um, ganz ohne KI. „Aktuelle Aufnahmen übernehmen“ trägt die gerade aufgenommenen Programme automatisch ein.
- **Falsch erkannt?** Im Dock setzt du mit **Falsch erkannt? Richtiges Spiel setzen …** die richtige Kategorie sofort. Auf Wunsch merkt sich das Plugin die Zuordnung für das aufgenommene Programm. Ohne „Merken“ stellt die KI danach 15 Minuten lang nicht um.
- **Programm-Hinweise:** Die KI erfährt, welche Programme OBS aufnimmt, samt aktuellem Fenstertitel. Bei Emulatoren wie Dolphin oder Citron steht dort meist das laufende Spiel.
- **Einzelne Quelle statt Programmausgabe:** Unter **Erkennung → Bildquelle** deine Spielaufnahme wählen. Dann lenken Webcam, Overlays und Browserfenster die KI nicht ab.

## Einstellungen

| Reiter | Einstellung | Bedeutung |
|---|---|---|
| Erkennung | Prüfintervall | Abstand zwischen zwei Prüfungen (Standard 60 s) |
| | Bildquelle | Programmausgabe oder eine einzelne Quelle. Die Spielaufnahme ist genauer, weil Webcam und Overlays nicht ablenken. |
| | Mindest-Sicherheit | Unsichere Erkennungen werden ignoriert |
| | Bestätigungen | So oft hintereinander muss ein neues Spiel erkannt werden (Standard 2) |
| | Ohne Spiel | z. B. `Just Chatting`, wenn kein Spiel zu sehen ist. Leer lassen, dann bleibt die Kategorie. |
| | Aktives Fenster mitsenden | Fenstertitel und Prozessname als Hinweis an die KI |
| Titel | Titel | KI schreibt ihn, eine Vorlage wird verwendet oder der Titel bleibt unverändert |
| | Stil für KI-Titel | Freitext, z. B. „Kurz und witzig, immer mit ‚!discord‘ am Ende“ |

## Datenschutz

- An den gewählten KI-Anbieter geht pro Prüfung ein verkleinertes Standbild deines Streams. Das ist genau das, was deine Zuschauer ohnehin sehen. Ist die Option aktiv, kommen Titel und Prozessname des aktiven Fensters dazu. Fenstertitel können private Infos enthalten, z. B. Browser-Tabs.
- **Kostenlose Kontingente** (z. B. Gemini Free Tier) dürfen die Anbieter laut ihren Bedingungen zur Verbesserung ihrer Dienste verwenden. Wer das nicht möchte, nimmt Ollama: Dann verlässt kein Bild deinen PC.
- API-Keys und der Twitch-Login liegen nur lokal in `%APPDATA%\obs-studio\plugin_config\ki-game-switcher\`. **Trennen** löscht den Twitch-Login.

## Fehlerbehebung

Meldungen erscheinen im Dock und im Verlauf, Details stehen im OBS-Log (**Hilfe → Protokolldateien**).

| Meldung | Lösung |
|---|---|
| `API-Key ungültig` | Key in den Einstellungen prüfen, dann „Modelle laden“ |
| `Modell nicht gefunden` | „Modelle laden“ und ein Modell aus der Liste wählen |
| `… ist gerade überlastet (HTTP 503)` / `antwortet nicht` | Der Anbieter ist ausgelastet. Das Ersatzmodell springt ein. Scheitert auch das, klappt es meist bei der nächsten Prüfung. |
| `Limit für … erreicht` | Kostenloses Kontingent aufgebraucht. Intervall erhöhen oder später wieder versuchen. |
| `Ollama nicht erreichbar` | Läuft Ollama? Ist das Modell geladen? |
| `Kein Bild` | Läuft das Spiel? Zeigt OBS die Spielaufnahme an? |
| `Twitch-Anmeldung abgelaufen` | Nach 30 Tagen ohne Nutzung neu verbinden |

## Selbst bauen

Voraussetzungen unter Windows: Visual Studio 2022 (oder Build Tools) mit C++-Workload und Windows SDK 10.0.22621, CMake ≥ 3.28.

```bash
cmake --preset windows-x64
```

```bash
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Die DLL landet in `build_x64/RelWithDebInfo/`. Für macOS und Linux gibt es die Presets `macos` und `ubuntu-x86_64`. Die GitHub-Actions-Workflows bauen alle drei Plattformen automatisch und hängen bei Tags die Pakete an ein Release.

Das Projekt basiert auf der offiziellen [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

## Lizenz

GPL-2.0-or-later, siehe [LICENSE](LICENSE).
