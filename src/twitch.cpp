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

#include "twitch.hpp"
#include "i18n.hpp"

#include "http.hpp"
#include "settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace kgs {

namespace {

constexpr const char *kScopes = "channel:manage:broadcast";

// Die Umgebungsvariablen sind nur für Tests gegen einen lokalen Fake-Server gedacht
std::string idUrl()
{
	const char *url = std::getenv("KGS_TWITCH_ID_URL");
	return url && *url ? url : "https://id.twitch.tv/oauth2";
}

std::string apiUrl()
{
	const char *url = std::getenv("KGS_TWITCH_API_URL");
	return url && *url ? url : "https://api.twitch.tv/helix";
}

QJsonObject parseObject(const std::string &body)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(body)).object();
}

std::string str(const QJsonObject &object, const char *key)
{
	return object.value(key).toString().toStdString();
}

std::string lower(const std::string &value)
{
	return QString::fromStdString(value).trimmed().toLower().toStdString();
}

std::string describe(const HttpResponse &response)
{
	if (!response.error.empty())
		return Trf("Twitch.NoConnection", {response.error});
	std::string message = str(parseObject(response.body), "message");
	return "HTTP " + std::to_string(response.status) + (message.empty() ? "" : " " + message);
}

} // namespace

Twitch::Twitch()
{
	load();
}

void Twitch::load()
{
	obs_data_t *data = obs_data_create_from_json_file_safe(configPath("twitch.json").c_str(), "bak");
	if (!data)
		return;
	clientId_ = obs_data_get_string(data, "client_id");
	accessToken_ = obs_data_get_string(data, "access_token");
	refreshToken_ = obs_data_get_string(data, "refresh_token");
	userId_ = obs_data_get_string(data, "user_id");
	login_ = obs_data_get_string(data, "login");
	obs_data_release(data);
	publishInfo();
}

void Twitch::publishInfo()
{
	std::lock_guard lock(infoMutex_);
	infoConnected_ = !accessToken_.empty() && !userId_.empty();
	infoLogin_ = login_;
}

void Twitch::save()
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "client_id", clientId_.c_str());
	obs_data_set_string(data, "access_token", accessToken_.c_str());
	obs_data_set_string(data, "refresh_token", refreshToken_.c_str());
	obs_data_set_string(data, "user_id", userId_.c_str());
	obs_data_set_string(data, "login", login_.c_str());
	os_mkdirs(configPath("").c_str());
	obs_data_save_json_safe(data, configPath("twitch.json").c_str(), "tmp", "bak");
	obs_data_release(data);
}

bool Twitch::connected() const
{
	std::lock_guard lock(infoMutex_);
	return infoConnected_;
}

std::string Twitch::login() const
{
	std::lock_guard lock(infoMutex_);
	return infoLogin_;
}

void Twitch::disconnect()
{
	std::lock_guard lock(mutex_);
	clientId_.clear();
	accessToken_.clear();
	refreshToken_.clear();
	userId_.clear();
	login_.clear();
	validated_ = false;
	categoryCache_.clear();
	os_unlink(configPath("twitch.json").c_str());
	publishInfo();
}

DeviceFlow Twitch::startDeviceFlow(const std::string &clientId)
{
	if (clientId.empty())
		throw TwitchError(Tr("Twitch.NoTwitchClientIDConfigured"));

	HttpResponse response = httpRequest("POST", idUrl() + "/device",
					    {"Content-Type: application/x-www-form-urlencoded"},
					    formEncode({{"client_id", clientId}, {"scopes", kScopes}}));
	if (response.status != 200)
		throw TwitchError(Tr("Twitch.TwitchRejectedSignIn") + describe(response));

	QJsonObject data = parseObject(response.body);
	DeviceFlow flow;
	flow.deviceCode = str(data, "device_code");
	flow.userCode = str(data, "user_code");
	flow.verificationUri = str(data, "verification_uri");
	flow.expiresIn = data.value("expires_in").toInt(1800);
	flow.interval = std::max(1, data.value("interval").toInt(5));
	return flow;
}

bool Twitch::waitForDeviceLogin(const std::string &clientId, const DeviceFlow &flow,
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
			httpRequest("POST", idUrl() + "/token", {"Content-Type: application/x-www-form-urlencoded"},
				    formEncode({{"client_id", clientId},
						{"scopes", kScopes},
						{"device_code", flow.deviceCode},
						{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}));
		if (response.status == 200) {
			QJsonObject data = parseObject(response.body);
			std::lock_guard lock(mutex_);
			clientId_ = clientId;
			accessToken_ = str(data, "access_token");
			refreshToken_ = str(data, "refresh_token");
			categoryCache_.clear();
			if (!validate())
				throw TwitchError(Tr("Twitch.NewTwitchTokenIsInvalid"));
			save();
			return true;
		}

		const std::string message = str(parseObject(response.body), "message");
		if (message == "authorization_pending")
			continue;
		if (message == "slow_down") {
			interval += 5;
			continue;
		}
		throw TwitchError(Tr("Twitch.TwitchSignInFailed") + describe(response));
	}
	return false;
}

bool Twitch::validate()
{
	HttpResponse response = httpRequest("GET", idUrl() + "/validate", {"Authorization: OAuth " + accessToken_});
	if (response.status == 401)
		return false;
	if (response.status != 200)
		throw TwitchError(Tr("Twitch.CouldntCheckTwitchToken") + describe(response));

	QJsonObject data = parseObject(response.body);
	userId_ = str(data, "user_id");
	login_ = str(data, "login");
	validatedAt_ = std::chrono::steady_clock::now();
	validated_ = true;
	publishInfo();
	return true;
}

void Twitch::refresh()
{
	HttpResponse response = httpRequest(
		"POST", idUrl() + "/token", {"Content-Type: application/x-www-form-urlencoded"},
		formEncode(
			{{"grant_type", "refresh_token"}, {"refresh_token", refreshToken_}, {"client_id", clientId_}}));
	if (response.status == 400 || response.status == 401) {
		disconnect();
		throw TwitchError(Tr("Twitch.TwitchSignInExpiredPlease"));
	}
	if (response.status != 200)
		throw TwitchError(Tr("Twitch.CouldntRenewTwitchToken") + describe(response));

	QJsonObject data = parseObject(response.body);
	accessToken_ = str(data, "access_token");
	if (data.contains("refresh_token"))
		refreshToken_ = str(data, "refresh_token");
	if (!validate())
		throw TwitchError(Tr("Twitch.RenewedTwitchTokenIsInvalid"));
	save();
}

QJsonObject Twitch::helix(const char *method, const std::string &path, const std::string &query,
			  const std::string &body)
{
	std::lock_guard lock(mutex_);
	if (accessToken_.empty() || userId_.empty())
		throw TwitchError(Tr("Twitch.NotConnectedToTwitch"));

	// Twitch verlangt, dass Tokens regelmäßig (mindestens stündlich) validiert werden
	if (!validated_ || std::chrono::steady_clock::now() - validatedAt_ > std::chrono::minutes(50)) {
		if (!validate())
			refresh();
	}

	const std::string url = apiUrl() + path + (query.empty() ? "" : "?" + query);
	for (int attempt = 0; attempt < 2; attempt++) {
		std::vector<std::string> headers{"Authorization: Bearer " + accessToken_, "Client-Id: " + clientId_};
		if (!body.empty())
			headers.push_back("Content-Type: application/json");
		HttpResponse response = httpRequest(method, url, headers, body);
		if (response.status == 401 && attempt == 0) {
			refresh();
			continue;
		}
		if (response.status < 200 || response.status >= 300)
			throw TwitchError(std::string("Twitch ") + method + " " + path + ": " + describe(response));
		return parseObject(response.body);
	}
	return {};
}

ChannelInfo Twitch::getChannel()
{
	std::lock_guard lock(mutex_);
	QJsonArray list = helix("GET", "/channels", "broadcaster_id=" + urlEncode(userId_)).value("data").toArray();
	QJsonObject channel = list.isEmpty() ? QJsonObject() : list.first().toObject();
	return {str(channel, "game_id"), str(channel, "game_name"), str(channel, "title")};
}

bool Twitch::isLive()
{
	std::lock_guard lock(mutex_);
	const QJsonArray streams = helix("GET", "/streams", "user_id=" + urlEncode(userId_)).value("data").toArray();
	return !streams.isEmpty() && streams.first().toObject().value("type").toString() == "live";
}

std::optional<Category> Twitch::findCategory(const std::string &name)
{
	std::lock_guard lock(mutex_);
	const std::string key = lower(name);
	if (auto cached = categoryCache_.find(key); cached != categoryCache_.end())
		return cached->second;

	QString cleaned = QString::fromStdString(name);
	cleaned.remove(QChar(0x2122)).remove(QChar(0x00AE)); // ™ ®
	const std::string trimmed = cleaned.simplified().toStdString();
	QJsonArray hits = helix("GET", "/games", "name=" + urlEncode(trimmed)).value("data").toArray();
	if (hits.isEmpty()) {
		QJsonArray search = helix("GET", "/search/categories", "first=10&query=" + urlEncode(trimmed))
					    .value("data")
					    .toArray();
		for (const QJsonValue hit : search) {
			if (lower(str(hit.toObject(), "name")) == key) {
				hits.append(hit);
				break;
			}
		}
		if (hits.isEmpty() && !search.isEmpty())
			hits.append(search.first());
	}
	// Twitch führt manche Spiele ohne führendes „The“
	if (hits.isEmpty() && cleaned.simplified().startsWith("The ", Qt::CaseInsensitive)) {
		std::optional<Category> withoutThe = findCategory(cleaned.simplified().mid(4).toStdString());
		categoryCache_[key] = withoutThe;
		return withoutThe;
	}

	std::optional<Category> result;
	if (!hits.isEmpty())
		result = Category{str(hits.first().toObject(), "id"), str(hits.first().toObject(), "name")};
	categoryCache_[key] = result;
	return result;
}

void Twitch::updateChannel(const std::string &gameId, const std::string &title, const std::vector<std::string> &tags)
{
	std::lock_guard lock(mutex_);
	QJsonObject body{{"game_id", QString::fromStdString(gameId)}};
	if (!title.empty())
		body["title"] = QString::fromStdString(title).left(140);
	if (!tags.empty()) {
		QJsonArray array;
		for (const std::string &tag : tags)
			array.append(QString::fromStdString(tag));
		body["tags"] = array;
	}
	helix("PATCH", "/channels", "broadcaster_id=" + urlEncode(userId_),
	      QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
}

void Twitch::updateTitle(const std::string &title)
{
	std::lock_guard lock(mutex_);
	QJsonObject body{{"title", QString::fromStdString(title).left(140)}};
	helix("PATCH", "/channels", "broadcaster_id=" + urlEncode(userId_),
	      QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
}

} // namespace kgs
