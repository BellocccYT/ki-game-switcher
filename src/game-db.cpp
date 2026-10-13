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

#include "game-db.hpp"

#include "http.hpp"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <map>
#include <mutex>

namespace kgs {

namespace {

constexpr int kShotWidth = 640;
constexpr int kShotHeight = 360;
constexpr int kGap = 8;
constexpr size_t kShotsPerGame = 2;

std::mutex g_cacheMutex;
std::map<std::string, std::optional<ReferenceGame>> g_cache;

// „ELDEN RING™“ → „elden ring“
QString normalized(const QString &name)
{
	QString text = name.toLower();
	text.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
	text.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
	return text.simplified();
}

// Gleicher Name, oder einer steckt im anderen und unterscheidet sich nur wenig (z. B. „… Enhanced“)
bool sameGame(const QString &query, const QString &candidate)
{
	if (query == candidate)
		return true;
	const QString &shorter = query.size() < candidate.size() ? query : candidate;
	const QString &longer = query.size() < candidate.size() ? candidate : query;
	return longer.startsWith(shorter + ' ') && shorter.size() * 10 >= longer.size() * 6;
}

// reachable wird false, wenn Steam nicht antwortet – das Ergebnis ist dann nicht endgültig
QJsonObject getJson(const std::string &url, bool &reachable)
{
	const HttpResponse response = httpRequest("GET", url, {"Accept: application/json"}, {}, 15);
	if (response.status != 200) {
		reachable = false;
		return {};
	}
	return QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();
}

std::optional<ReferenceGame> lookup(const std::string &name, bool &reachable)
{
	const QString query = normalized(QString::fromStdString(name));
	if (query.isEmpty())
		return std::nullopt;

	const QJsonObject search = getJson(
		"https://store.steampowered.com/api/storesearch/?l=english&cc=US&term=" + urlEncode(name), reachable);
	const QJsonArray items = search.value("items").toArray();
	QJsonObject best;
	for (const QJsonValue item : items) {
		const QJsonObject object = item.toObject();
		const QString candidate = normalized(object.value("name").toString());
		if (candidate == query) {
			best = object;
			break;
		}
		if (best.isEmpty() && sameGame(query, candidate))
			best = object;
	}
	if (best.isEmpty())
		return std::nullopt;

	const std::string appId = QString::number(best.value("id").toInteger()).toStdString();
	const QJsonObject details =
		getJson("https://store.steampowered.com/api/appdetails?l=english&appids=" + appId, reachable)
			.value(QString::fromStdString(appId))
			.toObject();
	const QJsonArray shots = details.value("data").toObject().value("screenshots").toArray();

	ReferenceGame game;
	game.name = best.value("name").toString().toStdString();
	for (const QJsonValue shot : shots) {
		if (game.screenshots.size() >= kShotsPerGame)
			break;
		const std::string url = shot.toObject().value("path_full").toString().toStdString();
		if (url.empty())
			continue;
		const HttpResponse image = httpRequest("GET", url, {}, {}, 15);
		QImage decoded;
		if (image.status == 200 && decoded.loadFromData(QByteArray::fromStdString(image.body)))
			game.screenshots.push_back(decoded);
	}
	if (game.screenshots.empty())
		return std::nullopt;
	return game;
}

} // namespace

std::optional<ReferenceGame> steamReference(const std::string &name)
{
	const std::string key = normalized(QString::fromStdString(name)).toStdString();
	{
		std::lock_guard lock(g_cacheMutex);
		auto it = g_cache.find(key);
		if (it != g_cache.end())
			return it->second;
	}
	bool reachable = true;
	std::optional<ReferenceGame> game = lookup(name, reachable);
	if (game || reachable) {
		std::lock_guard lock(g_cacheMutex);
		g_cache[key] = game;
	}
	return game;
}

QImage referenceSheet(const ReferenceGame &game)
{
	const int count = std::max<int>(1, static_cast<int>(game.screenshots.size()));
	QImage sheet(count * kShotWidth + (count - 1) * kGap, kShotHeight, QImage::Format_RGB32);
	sheet.fill(QColor(24, 24, 28));
	QPainter painter(&sheet);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	int left = 0;
	for (const QImage &shot : game.screenshots) {
		const QImage scaled =
			shot.scaled(kShotWidth, kShotHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		painter.drawImage(left + (kShotWidth - scaled.width()) / 2, (kShotHeight - scaled.height()) / 2,
				  scaled);
		left += kShotWidth + kGap;
	}
	painter.end();
	return sheet;
}

} // namespace kgs
