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

#include "title.hpp"

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace kgs {

namespace {

QString qstr(const std::string &value)
{
	return QString::fromStdString(value).simplified();
}

// Trennzeichen, die nach einem leeren Platzhalter übrig bleiben würden
const QString kSeparators = QStringLiteral(" -–—|·:,/");

QString trimSeparators(QString text)
{
	text = text.simplified();
	while (!text.isEmpty() && kSeparators.contains(text.front()))
		text.remove(0, 1);
	while (!text.isEmpty() && kSeparators.contains(text.back()))
		text.chop(1);
	return text.trimmed();
}

} // namespace

int coreTitleLimit(const Settings &settings)
{
	int used = 0;
	if (!qstr(settings.titlePrefix).isEmpty())
		used += qstr(settings.titlePrefix).size() + 1;
	if (!qstr(settings.titleSuffix).isEmpty())
		used += qstr(settings.titleSuffix).size() + 1;
	return std::max(20, kTwitchTitleLimit - used);
}

std::string stripEmojis(const std::string &text)
{
	const QString input = QString::fromStdString(text);
	QString output;
	for (qsizetype i = 0; i < input.size(); i++) {
		char32_t code = input[i].unicode();
		qsizetype length = 1;
		if (input[i].isHighSurrogate() && i + 1 < input.size() && input[i + 1].isLowSurrogate()) {
			code = QChar::surrogateToUcs4(input[i], input[i + 1]);
			length = 2;
		}
		const bool emoji = (code >= 0x1F000 && code <= 0x1FAFF) || (code >= 0x2600 && code <= 0x27BF) ||
				   (code >= 0x2B00 && code <= 0x2BFF) || code == 0xFE0F || code == 0x200D ||
				   (code >= 0xE0020 && code <= 0xE007F);
		if (!emoji)
			output += input.mid(i, length);
		i += length - 1;
	}
	return output.simplified().toStdString();
}

std::string titleLanguageName(const Settings &settings)
{
	return settings.titleLanguage == "en" ? "English" : "German";
}

std::vector<std::string> cleanTags(const std::vector<std::string> &tags)
{
	std::vector<std::string> result;
	QStringList seen;
	for (const std::string &tag : tags) {
		QString clean = QString::fromStdString(tag);
		clean.remove(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]")));
		clean = clean.left(25);
		if (clean.isEmpty() || seen.contains(clean, Qt::CaseInsensitive))
			continue;
		seen << clean;
		result.push_back(clean.toStdString());
		if (result.size() == 10)
			break;
	}
	return result;
}

std::string composeTitle(const Settings &settings, const std::string &core, int limit)
{
	const QString prefix = qstr(settings.titlePrefix);
	const QString suffix = qstr(settings.titleSuffix);
	QString body = qstr(settings.titleEmojis ? core : stripEmojis(core));
	// Hat die KI die Vorgabe schon übernommen, nicht doppelt anhängen
	if (!prefix.isEmpty() && body.startsWith(prefix, Qt::CaseInsensitive))
		body = body.mid(prefix.size()).trimmed();
	if (!suffix.isEmpty() && body.endsWith(suffix, Qt::CaseInsensitive))
		body.chop(suffix.size());
	body = body.trimmed();

	const int coreLimit = std::max(20, coreTitleLimit(settings) - (kTwitchTitleLimit - limit));
	if (body.size() > coreLimit)
		body = body.left(coreLimit - 1).trimmed() + QStringLiteral("…");

	QStringList parts;
	for (const QString &part : {prefix, body, suffix}) {
		if (!part.isEmpty())
			parts << part;
	}
	return parts.join(' ').left(limit).toStdString();
}

std::string fillTemplate(const std::string &templ, const std::string &game, const std::string &scene,
			 const std::vector<std::string> &keywords)
{
	QStringList words;
	for (const std::string &keyword : keywords) {
		if (!qstr(keyword).isEmpty())
			words << qstr(keyword);
	}
	const QString values[] = {qstr(game), qstr(scene), words.join(", ")};
	const QString names[] = {QStringLiteral("{game}"), QStringLiteral("{scene}"), QStringLiteral("{keywords}")};

	QString result = QString::fromStdString(templ);
	for (int i = 0; i < 3; i++) {
		if (!values[i].isEmpty()) {
			result.replace(names[i], values[i], Qt::CaseInsensitive);
			continue;
		}
		// Leerer Platzhalter: samt davorstehendem Trennzeichen entfernen („{game} – {scene}“ → „{game}“)
		const QRegularExpression withSeparator(QStringLiteral("\\s*[-–—|·:,/]?\\s*") +
							       QRegularExpression::escape(names[i]),
						       QRegularExpression::CaseInsensitiveOption);
		result.remove(withSeparator);
	}
	return trimSeparators(result).toStdString();
}

std::string titleStyleFor(const Settings &settings)
{
	const std::string preset = titlePreset(settings.titlePreset).style;
	const std::string extra = qstr(settings.titleStyle).toStdString();
	if (preset.empty())
		return extra;
	if (extra.empty())
		return preset;
	return preset + " Zusätzlich: " + extra;
}

} // namespace kgs
