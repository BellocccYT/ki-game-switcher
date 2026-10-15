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

#include "i18n.hpp"

#include <obs-module.h>
#include <util/text-lookup.h>

#include <mutex>

namespace kgs {

namespace {

std::mutex g_mutex;
lookup_t *g_lookup = nullptr;

std::string localeFor(const std::string &language)
{
	if (language == "de")
		return "de-DE";
	if (language == "en")
		return "en-US";
	const char *obsLocale = obs_get_locale();
	return obsLocale && std::string(obsLocale).rfind("de", 0) == 0 ? "de-DE" : "en-US";
}

} // namespace

void i18nInit(const std::string &language)
{
	lookup_t *lookup = obs_module_load_locale(obs_current_module(), "en-US", localeFor(language).c_str());
	std::lock_guard lock(g_mutex);
	// Die alte Tabelle bleibt bestehen: Zeiger aus Tr() dürfen nicht plötzlich ungültig werden.
	if (lookup)
		g_lookup = lookup;
}

void i18nFree()
{
	std::lock_guard lock(g_mutex);
	if (g_lookup)
		text_lookup_destroy(g_lookup);
	g_lookup = nullptr;
}

const char *Tr(const char *key)
{
	std::lock_guard lock(g_mutex);
	const char *text = nullptr;
	if (g_lookup && text_lookup_getstr(g_lookup, key, &text))
		return text;
	return key;
}

std::string Trf(const char *key, std::initializer_list<std::string> args)
{
	// In einem Durchgang, damit ein „%2“ in einem Argument nicht selbst ersetzt wird
	const std::string pattern = Tr(key);
	std::string text;
	for (size_t i = 0; i < pattern.size(); i++) {
		const char next = i + 1 < pattern.size() ? pattern[i + 1] : '\0';
		if (pattern[i] == '%' && next >= '1' && next <= '9' && static_cast<size_t>(next - '1') < args.size()) {
			text += *(args.begin() + (next - '1'));
			i++;
		} else {
			text += pattern[i];
		}
	}
	return text;
}

} // namespace kgs
