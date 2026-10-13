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
#include <cstdio>
#include <filesystem>

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

struct DownloadState {
	std::FILE *file = nullptr;
	const std::function<void(int)> *progress = nullptr;
	int lastPercent = -2;
};

size_t fileWriteCallback(char *data, size_t size, size_t count, void *user)
{
	return std::fwrite(data, size, count, static_cast<DownloadState *>(user)->file) * size;
}

int downloadProgressCallback(void *user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t)
{
	if (g_abort)
		return 1;
	auto *state = static_cast<DownloadState *>(user);
	const int percent = total > 0 ? static_cast<int>(now * 100 / total) : -1;
	if (percent != state->lastPercent && *state->progress) {
		state->lastPercent = percent;
		(*state->progress)(percent);
	}
	return 0;
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

std::string httpDownload(const std::string &url, const std::string &targetPath,
			 const std::function<void(int)> &progress)
{
	namespace fs = std::filesystem;
	const fs::path target = fs::u8path(targetPath);
	const fs::path partial = fs::u8path(targetPath + ".part");
	std::error_code ignored;
	fs::create_directories(target.parent_path(), ignored);

	DownloadState state;
	state.progress = &progress;
#ifdef _WIN32
	state.file = _wfopen(partial.c_str(), L"wb");
#else
	state.file = std::fopen(partial.c_str(), "wb");
#endif
	if (!state.file)
		return "Datei kann nicht angelegt werden: " + partial.u8string();

	CURL *curl = curl_easy_init();
	char errorBuffer[CURL_ERROR_SIZE] = {};
	const std::string userAgent = std::string(PLUGIN_NAME) + "/" + PLUGIN_VERSION;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	// Abbruch nur, wenn die Verbindung eine Minute lang praktisch steht – große Dateien brauchen Zeit
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, downloadProgressCallback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
#ifdef _WIN32
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif
	const CURLcode code = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	std::fclose(state.file);

	if (code != CURLE_OK) {
		fs::remove(partial, ignored);
		return errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
	}
	fs::rename(partial, target, ignored);
	if (ignored) {
		fs::remove(partial, ignored);
		return "Datei kann nicht gespeichert werden: " + target.u8string();
	}
	return {};
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
