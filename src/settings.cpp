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

#include "settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>

namespace kgs {

namespace {

// Client-ID der öffentlichen Twitch-App dieses Plugins (Client-Typ „Öffentlich“).
// Eine Client-ID ist nicht geheim; Nutzer können in den Einstellungen eine eigene eintragen.
constexpr const char *kBuiltinTwitchClientId = "qecldbsld4yvnhb4r976a7jrctjt6z";

const ProviderInfo kProviders[kProviderCount] = {
	{"gemini", "Google Gemini", "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-3.8-flash",
	 "gemini-3.5-flash-lite", true,
	 "Key erstellen: <a href=\"https://aistudio.google.com/apikey\">aistudio.google.com/apikey</a>. In der EU, "
	 "der Schweiz und Großbritannien erlaubt Google für Programme wie dieses nur die kostenpflichtige API "
	 "(Google-Cloud-Projekt mit Abrechnung)."},
	{"openrouter", "OpenRouter (kostenlose Modelle)", "https://openrouter.ai/api/v1", "openrouter/free", "", true,
	 "Key: <a href=\"https://openrouter.ai/keys\">openrouter.ai/keys</a>. Ohne Guthaben nur rund 50 Anfragen "
	 "pro Tag – stell das Prüfintervall dann auf mindestens 5 Minuten."},
	{"ollama", "Ollama (lokal, ohne Key)", "http://localhost:11434/v1", "qwen3-vl:4b-instruct", "", false,
	 "Läuft komplett auf deinem PC: <a href=\"https://ollama.com\">ollama.com</a> installieren und ein Bildmodell "
	 "laden: <code>ollama pull qwen3-vl:4b-instruct</code> (3,3 GB). Braucht ca. 4 GB Grafikspeicher und rechnet pro "
	 "Prüfung einige Sekunden auf der Grafikkarte."},
	{"custom", "Eigener Server (OpenAI-kompatibel)", "", "", "", false,
	 "Jeder Dienst mit OpenAI-kompatibler Schnittstelle, z. B. LM Studio. Server-URL inklusive <code>/v1</code> "
	 "eintragen."},
	{"claude", "Anthropic Claude (kostenpflichtig)", "https://api.anthropic.com", "claude-opus-5-5", "", true,
	 "Sehr zuverlässige Erkennung, kostet aber pro Anfrage. Key: "
	 "<a href=\"https://platform.claude.com\">platform.claude.com</a>"},
};

const char *titleModeId(TitleMode mode)
{
	switch (mode) {
	case TitleMode::Template:
		return "template";
	case TitleMode::Keep:
		return "keep";
	default:
		return "ai";
	}
}

TitleMode titleModeFromId(const std::string &id)
{
	if (id == "template")
		return TitleMode::Template;
	if (id == "keep")
		return TitleMode::Keep;
	return TitleMode::Ai;
}

} // namespace

const ProviderInfo &providerInfo(Provider provider)
{
	return kProviders[static_cast<int>(provider)];
}

std::string Settings::activeModel() const
{
	const std::string &model = activeProvider().model;
	return model.empty() ? providerInfo(provider).defaultModel : model;
}

std::string Settings::activeBaseUrl() const
{
	const std::string &url = activeProvider().baseUrl;
	return url.empty() ? providerInfo(provider).baseUrl : url;
}

std::string Settings::effectiveTwitchClientId() const
{
	return twitchClientId.empty() ? kBuiltinTwitchClientId : twitchClientId;
}

std::string configPath(const char *file)
{
	char *path = obs_module_config_path(file);
	std::string result = path ? path : "";
	bfree(path);
	return result;
}

Settings loadSettings()
{
	Settings s;
	for (int i = 0; i < kProviderCount; i++)
		s.providers[i].fallbackModel = kProviders[i].defaultFallbackModel;
	obs_data_t *data = obs_data_create_from_json_file_safe(configPath("settings.json").c_str(), "bak");
	if (!data)
		return s;

	auto getString = [data](const char *key, std::string &out) {
		if (obs_data_has_user_value(data, key))
			out = obs_data_get_string(data, key);
	};
	auto getInt = [data](const char *key, int &out, int min, int max) {
		if (obs_data_has_user_value(data, key))
			out = std::clamp(static_cast<int>(obs_data_get_int(data, key)), min, max);
	};
	auto getBool = [data](const char *key, bool &out) {
		if (obs_data_has_user_value(data, key))
			out = obs_data_get_bool(data, key);
	};

	std::string providerId;
	getString("provider", providerId);
	for (int i = 0; i < kProviderCount; i++) {
		if (providerId == kProviders[i].id)
			s.provider = static_cast<Provider>(i);
	}

	obs_data_t *providers = obs_data_get_obj(data, "providers");
	for (int i = 0; providers && i < kProviderCount; i++) {
		obs_data_t *p = obs_data_get_obj(providers, kProviders[i].id);
		if (!p)
			continue;
		s.providers[i].apiKey = obs_data_get_string(p, "api_key");
		s.providers[i].model = obs_data_get_string(p, "model");
		// Fehlt der Eintrag (ältere Konfiguration), bleibt das Standard-Ersatzmodell
		if (obs_data_has_user_value(p, "fallback_model"))
			s.providers[i].fallbackModel = obs_data_get_string(p, "fallback_model");
		s.providers[i].baseUrl = obs_data_get_string(p, "base_url");
		obs_data_release(p);
	}
	obs_data_release(providers);

	getString("twitch_client_id", s.twitchClientId);
	getBool("enabled", s.enabled);
	getBool("only_live", s.onlyLive);
	getInt("interval", s.interval, 20, 3600);
	getString("source", s.sourceName);
	getInt("image_width", s.imageWidth, 512, 1280);
	getInt("min_confidence", s.minConfidence, 0, 100);
	getInt("confirmations", s.confirmations, 1, 10);
	getString("idle_category", s.idleCategory);
	getBool("send_window_hint", s.sendWindowHint);
	getBool("dock_introduced", s.dockIntroduced);

	obs_data_array_t *rules = obs_data_get_array(data, "rules");
	for (size_t i = 0; rules && i < obs_data_array_count(rules); i++) {
		obs_data_t *rule = obs_data_array_item(rules, i);
		GameRule entry{obs_data_get_string(rule, "executable"), obs_data_get_string(rule, "category")};
		if (!entry.executable.empty() && !entry.category.empty())
			s.rules.push_back(entry);
		obs_data_release(rule);
	}
	obs_data_array_release(rules);

	std::string titleMode;
	getString("title_mode", titleMode);
	if (!titleMode.empty())
		s.titleMode = titleModeFromId(titleMode);
	getString("title_template", s.titleTemplate);
	getString("title_style", s.titleStyle);

	obs_data_release(data);
	return s;
}

void saveSettings(const Settings &s)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "provider", providerInfo(s.provider).id);

	obs_data_t *providers = obs_data_create();
	for (int i = 0; i < kProviderCount; i++) {
		obs_data_t *p = obs_data_create();
		obs_data_set_string(p, "api_key", s.providers[i].apiKey.c_str());
		obs_data_set_string(p, "model", s.providers[i].model.c_str());
		obs_data_set_string(p, "fallback_model", s.providers[i].fallbackModel.c_str());
		obs_data_set_string(p, "base_url", s.providers[i].baseUrl.c_str());
		obs_data_set_obj(providers, kProviders[i].id, p);
		obs_data_release(p);
	}
	obs_data_set_obj(data, "providers", providers);
	obs_data_release(providers);

	obs_data_set_string(data, "twitch_client_id", s.twitchClientId.c_str());
	obs_data_set_bool(data, "enabled", s.enabled);
	obs_data_set_bool(data, "only_live", s.onlyLive);
	obs_data_set_int(data, "interval", s.interval);
	obs_data_set_string(data, "source", s.sourceName.c_str());
	obs_data_set_int(data, "image_width", s.imageWidth);
	obs_data_set_int(data, "min_confidence", s.minConfidence);
	obs_data_set_int(data, "confirmations", s.confirmations);
	obs_data_set_string(data, "idle_category", s.idleCategory.c_str());
	obs_data_set_bool(data, "send_window_hint", s.sendWindowHint);
	obs_data_set_bool(data, "dock_introduced", s.dockIntroduced);

	obs_data_array_t *rules = obs_data_array_create();
	for (const GameRule &entry : s.rules) {
		obs_data_t *rule = obs_data_create();
		obs_data_set_string(rule, "executable", entry.executable.c_str());
		obs_data_set_string(rule, "category", entry.category.c_str());
		obs_data_array_push_back(rules, rule);
		obs_data_release(rule);
	}
	obs_data_set_array(data, "rules", rules);
	obs_data_array_release(rules);
	obs_data_set_string(data, "title_mode", titleModeId(s.titleMode));
	obs_data_set_string(data, "title_template", s.titleTemplate.c_str());
	obs_data_set_string(data, "title_style", s.titleStyle.c_str());

	os_mkdirs(configPath("").c_str());
	obs_data_save_json_safe(data, configPath("settings.json").c_str(), "tmp", "bak");
	obs_data_release(data);
}

} // namespace kgs
