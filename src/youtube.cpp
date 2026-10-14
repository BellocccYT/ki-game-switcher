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

#include "youtube.hpp"

#include "http.hpp"
#include "settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace kgs {

namespace {

constexpr const char *kScope = "https://www.googleapis.com/auth/youtube";

// Die Umgebungsvariablen sind nur für Tests gegen einen lokalen Fake-Server gedacht
std::string oauthUrl()
{
	const char *url = std::getenv("KGS_GOOGLE_OAUTH_URL");
	return url && *url ? url : "https://oauth2.googleapis.com";
}

std::string apiUrl()
{
	const char *url = std::getenv("KGS_YOUTUBE_API_URL");
	return url && *url ? url : "https://www.googleapis.com/youtube/v3";
}

QJsonObject parseObject(const std::string &body)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(body)).object();
}

std::string str(const QJsonObject &object, const char *key)
{
	return object.value(key).toString().toStdString();
}

std::string describe(const HttpResponse &response)
{
	if (!response.error.empty())
		return "keine Verbindung zu Google (" + response.error + ")";
	const QJsonObject body = parseObject(response.body);
	QString message = body.value("error").isObject() ? body.value("error").toObject().value("message").toString()
							 : body.value("error_description").toString();
	if (message.isEmpty())
		message = body.value("error").toString();
	return "HTTP " + std::to_string(response.status) + (message.isEmpty() ? "" : " " + message.toStdString());
}

} // namespace

YouTube::YouTube()
{
	load();
}

void YouTube::load()
{
	obs_data_t *data = obs_data_create_from_json_file_safe(configPath("youtube.json").c_str(), "bak");
	if (!data)
		return;
	clientId_ = obs_data_get_string(data, "client_id");
	clientSecret_ = obs_data_get_string(data, "client_secret");
	accessToken_ = obs_data_get_string(data, "access_token");
	refreshToken_ = obs_data_get_string(data, "refresh_token");
	channel_ = obs_data_get_string(data, "channel");
	obs_data_release(data);
	publishInfo();
}

void YouTube::save()
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "client_id", clientId_.c_str());
	obs_data_set_string(data, "client_secret", clientSecret_.c_str());
	obs_data_set_string(data, "access_token", accessToken_.c_str());
	obs_data_set_string(data, "refresh_token", refreshToken_.c_str());
	obs_data_set_string(data, "channel", channel_.c_str());
	os_mkdirs(configPath("").c_str());
	obs_data_save_json_safe(data, configPath("youtube.json").c_str(), "tmp", "bak");
	obs_data_release(data);
}

void YouTube::publishInfo()
{
	std::lock_guard lock(infoMutex_);
	infoConnected_ = !refreshToken_.empty();
	infoChannel_ = channel_;
}

bool YouTube::connected() const
{
	std::lock_guard lock(infoMutex_);
	return infoConnected_;
}

std::string YouTube::channel() const
{
	std::lock_guard lock(infoMutex_);
	return infoChannel_;
}

void YouTube::disconnect()
{
	std::lock_guard lock(mutex_);
	clientId_.clear();
	clientSecret_.clear();
	accessToken_.clear();
	refreshToken_.clear();
	channel_.clear();
	os_unlink(configPath("youtube.json").c_str());
	publishInfo();
}

DeviceFlow YouTube::startDeviceFlow(const std::string &clientId)
{
	if (clientId.empty())
		throw YouTubeError("Keine YouTube-Client-ID eingetragen (Einstellungen → YouTube)");
	HttpResponse response = httpRequest("POST", oauthUrl() + "/device/code",
					    {"Content-Type: application/x-www-form-urlencoded"},
					    formEncode({{"client_id", clientId}, {"scope", kScope}}));
	if (response.status != 200)
		throw YouTubeError("Google lehnt die Anmeldung ab: " + describe(response));

	QJsonObject data = parseObject(response.body);
	DeviceFlow flow;
	flow.deviceCode = str(data, "device_code");
	flow.userCode = str(data, "user_code");
	flow.verificationUri = str(data, "verification_url");
	if (flow.verificationUri.empty())
		flow.verificationUri = str(data, "verification_uri");
	flow.expiresIn = data.value("expires_in").toInt(1800);
	flow.interval = std::max(1, data.value("interval").toInt(5));
	return flow;
}

bool YouTube::waitForDeviceLogin(const std::string &clientId, const std::string &clientSecret, const DeviceFlow &flow,
				 const std::function<bool()> &cancelled)
{
	using namespace std::chrono;
	const auto deadline = steady_clock::now() + seconds(flow.expiresIn);
	int interval = flow.interval;

	while (steady_clock::now() < deadline) {
		for (int i = 0; i < interval * 10; i++) {
			if (cancelled())
				return false;
			std::this_thread::sleep_for(milliseconds(100));
		}
		HttpResponse response =
			httpRequest("POST", oauthUrl() + "/token", {"Content-Type: application/x-www-form-urlencoded"},
				    formEncode({{"client_id", clientId},
						{"client_secret", clientSecret},
						{"device_code", flow.deviceCode},
						{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}));
		if (response.status == 200) {
			QJsonObject data = parseObject(response.body);
			std::lock_guard lock(mutex_);
			clientId_ = clientId;
			clientSecret_ = clientSecret;
			accessToken_ = str(data, "access_token");
			refreshToken_ = str(data, "refresh_token");
			if (refreshToken_.empty())
				throw YouTubeError(
					"Google hat kein dauerhaftes Token geliefert – bitte erneut verbinden");
			// Kanalname für die Anzeige
			const QJsonArray channels =
				api("GET", "/channels", "part=snippet&mine=true").value("items").toArray();
			channel_ = channels.isEmpty()
					   ? std::string("YouTube-Kanal")
					   : str(channels.first().toObject().value("snippet").toObject(), "title");
			save();
			publishInfo();
			return true;
		}

		const std::string error = str(parseObject(response.body), "error");
		if (error == "authorization_pending")
			continue;
		if (error == "slow_down") {
			interval += 5;
			continue;
		}
		if (error == "access_denied")
			throw YouTubeError("YouTube-Anmeldung abgelehnt");
		throw YouTubeError("YouTube-Anmeldung fehlgeschlagen: " + describe(response));
	}
	return false;
}

void YouTube::refresh()
{
	HttpResponse response = httpRequest("POST", oauthUrl() + "/token",
					    {"Content-Type: application/x-www-form-urlencoded"},
					    formEncode({{"grant_type", "refresh_token"},
							{"refresh_token", refreshToken_},
							{"client_id", clientId_},
							{"client_secret", clientSecret_}}));
	if (response.status == 400 || response.status == 401) {
		disconnect();
		throw YouTubeError("YouTube-Anmeldung abgelaufen – bitte neu verbinden");
	}
	if (response.status != 200)
		throw YouTubeError("YouTube-Token konnte nicht erneuert werden: " + describe(response));
	accessToken_ = str(parseObject(response.body), "access_token");
	save();
}

QJsonObject YouTube::api(const char *method, const std::string &path, const std::string &query, const std::string &body)
{
	std::lock_guard lock(mutex_);
	if (refreshToken_.empty())
		throw YouTubeError("Nicht mit YouTube verbunden");
	if (accessToken_.empty())
		refresh();

	const std::string url = apiUrl() + path + (query.empty() ? "" : "?" + query);
	for (int attempt = 0; attempt < 2; attempt++) {
		std::vector<std::string> headers{"Authorization: Bearer " + accessToken_};
		if (!body.empty())
			headers.push_back("Content-Type: application/json");
		HttpResponse response = httpRequest(method, url, headers, body);
		if (response.status == 401 && attempt == 0) {
			refresh();
			continue;
		}
		if (response.status == 403 && QString::fromStdString(response.body).contains("quota"))
			throw YouTubeError("YouTube-Tageslimit deines Google-Projekts erreicht");
		if (response.status < 200 || response.status >= 300)
			throw YouTubeError(std::string("YouTube ") + method + " " + path + ": " + describe(response));
		return parseObject(response.body);
	}
	return {};
}

std::string YouTube::activeBroadcast()
{
	const QJsonArray items =
		api("GET", "/liveBroadcasts", "part=id&broadcastStatus=active&broadcastType=all&maxResults=1")
			.value("items")
			.toArray();
	return items.isEmpty() ? std::string() : str(items.first().toObject(), "id");
}

bool YouTube::isLive()
{
	return !activeBroadcast().empty();
}

bool YouTube::updateTitle(const std::string &title)
{
	std::lock_guard lock(mutex_);
	const std::string id = activeBroadcast();
	if (id.empty())
		return false;

	// videos.update überschreibt das ganze Snippet – deshalb Beschreibung, Tags und Kategorie übernehmen
	const QJsonArray videos = api("GET", "/videos", "part=snippet&id=" + urlEncode(id)).value("items").toArray();
	if (videos.isEmpty())
		return false;
	const QJsonObject current = videos.first().toObject().value("snippet").toObject();
	QJsonObject snippet;
	for (const char *key : {"description", "tags", "categoryId", "defaultLanguage", "defaultAudioLanguage"}) {
		if (current.contains(key))
			snippet[key] = current.value(key);
	}
	if (!snippet.contains("categoryId"))
		snippet["categoryId"] = "20"; // Gaming
	snippet["title"] = QString::fromStdString(title).left(kYouTubeTitleLimit);

	const QJsonObject body{{"id", QString::fromStdString(id)}, {"snippet", snippet}};
	api("PUT", "/videos", "part=snippet", QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
	return true;
}

} // namespace kgs
