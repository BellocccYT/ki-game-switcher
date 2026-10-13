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

struct Detection {
	bool isGame = false;
	std::string game;
	std::string category; // Name der Twitch-Kategorie laut KI
	double confidence = 0.0;
	std::string reason;
	std::string model; // Modell, das tatsächlich geantwortet hat
};

// Alle Funktionen blockieren und werfen std::runtime_error mit einer lesbaren deutschen Meldung.
Detection detectGame(const AiTarget &target, const EncodedImage &image, const std::string &windowHint);
std::string writeTitle(const AiTarget &target, const std::string &category, const std::string &style,
		       const std::string &previousTitle);
std::vector<std::string> listModels(const AiTarget &target);

} // namespace kgs
