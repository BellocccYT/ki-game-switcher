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

#include "http.hpp"

#include <plugin-support.h>

#include <curl/curl.h>

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <atomic>

namespace kgs {

namespace {

std::atomic<bool> g_abort{false};

size_t writeCallback(char *data, size_t size, size_t count, void *user)
{
	static_cast<std::string *>(user)->append(data, size * count);
	return size * count;
}

int progressCallback(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return g_abort ? 1 : 0;
}

} // namespace

void httpGlobalInit()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

void httpGlobalCleanup()
{
	curl_global_cleanup();
}

void httpAbortAll()
{
	g_abort = true;
}

HttpResponse httpRequest(const std::string &method, const std::string &url, const std::vector<std::string> &headers,
			 const std::string &body, long timeoutSeconds)
{
	HttpResponse response;
	if (g_abort) {
		response.error = "abgebrochen";
		return response;
	}

	CURL *curl = curl_easy_init();
	if (!curl) {
		response.error = "curl_easy_init fehlgeschlagen";
		return response;
	}

	curl_slist *headerList = nullptr;
	for (const std::string &header : headers)
		headerList = curl_slist_append(headerList, header.c_str());

	char errorBuffer[CURL_ERROR_SIZE] = {};
	const std::string userAgent = std::string(PLUGIN_NAME) + "/" + PLUGIN_VERSION;

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	if (method == "GET") {
		curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	} else {
		if (method != "POST")
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
#ifdef _WIN32
	// Wie OBS selbst: fehlende Sperrlisten-Server sollen HTTPS nicht blockieren
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif

	CURLcode code = curl_easy_perform(curl);
	if (code == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	} else {
		response.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
		response.timedOut = code == CURLE_OPERATION_TIMEDOUT;
	}

	curl_slist_free_all(headerList);
	curl_easy_cleanup(curl);
	return response;
}

std::string urlEncode(const std::string &value)
{
	return QUrl::toPercentEncoding(QString::fromStdString(value)).toStdString();
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>> &fields)
{
	std::string result;
	for (const auto &[key, value] : fields) {
		if (!result.empty())
			result += '&';
		result += urlEncode(key) + '=' + urlEncode(value);
	}
	return result;
}

} // namespace kgs
