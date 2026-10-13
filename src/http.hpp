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

#include <string>
#include <utility>
#include <vector>

namespace kgs {

struct HttpResponse {
	long status = 0;
	std::string body;
	std::string error; // Transportfehler (keine Verbindung, Timeout, abgebrochen); leer bei Erfolg
	bool timedOut = false;
};

// Blockierender HTTP-Aufruf – nur aus Hintergrund-Threads verwenden.
HttpResponse httpRequest(const std::string &method, const std::string &url, const std::vector<std::string> &headers,
			 const std::string &body = {}, long timeoutSeconds = 30);

std::string urlEncode(const std::string &value);
std::string formEncode(const std::vector<std::pair<std::string, std::string>> &fields);

void httpGlobalInit();
void httpGlobalCleanup();
// Bricht laufende und künftige Anfragen ab (beim Beenden von OBS).
void httpAbortAll();

} // namespace kgs
