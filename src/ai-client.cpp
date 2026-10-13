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

#include "ai-client.hpp"

#include "http.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace kgs {

namespace {

const char *kDetectSystem =
	R"(You identify which video game a streamer is currently playing, from a screenshot of their live stream output.

- Judge by the image: HUD, menus, art style, characters, logos, loading screens. Read visible text such as the game's title, logo or HUD labels to pin down the exact game.
- Stream overlays (webcam, chat box, alerts, borders, follower goals) are not part of the game - ignore them.
- Only actual gameplay, or the game's own menus and loading screens, counts. Videos of games (YouTube, Twitch, TikTok players), thumbnails, store pages, launchers, wallpapers or game art shown in a browser are NOT gameplay - set is_game to false for them.
- Set is_game to false when no game is visible: desktop, browser, Discord, OBS, video player, "starting soon" / "be right back" screens, or just a camera.
- You may get hints: the applications OBS is capturing (executable and window title) and the active window. The executable or window title of a captured game is strong evidence when the image agrees. For emulators (e.g. Dolphin, Citron, Ryujinx, RetroArch, PCSX2, RPCS3) the window title usually names the running game. A browser, Discord or OBS is not a game. The active window can be misleading, and a console game via capture card has no executable at all.
- If you cannot name the exact game, give a low confidence instead of guessing a similar-looking one.
- twitch_category: the exact name of the matching category in Twitch's directory, e.g. "Counter-Strike", "League of Legends", "Minecraft", "Grand Theft Auto V", "Elden Ring". Use the specific edition or sequel you can see. Empty string when is_game is false.
- game: the game's common name. Empty string when is_game is false.
- confidence: 0.0 to 1.0 - how sure you are about your answer. Stay below 0.5 when you are choosing between several similar games.
- reason: one short sentence naming the visual evidence.)";

const char *kDetectJsonFormat =
	R"(Reply with only this JSON object, no markdown and no other text: {"is_game": true or false, "game": "...", "twitch_category": "...", "confidence": 0.0, "reason": "..."})";

const char *kTitleSystem = R"(You write Twitch stream titles for a streamer who just switched games.
Follow the streamer's style instructions. The title must be at most 140 characters, a single line, without surrounding quotes.)";

const char *kTitleJsonFormat = R"(Reply with only this JSON object, no markdown and no other text: {"title": "..."})";

// Fehler mit der Info, ob ein anderes Modell es noch versuchen sollte (Überlastung, Limit, Zeitüberschreitung)
class AiError : public std::runtime_error {
public:
	AiError(const std::string &message, bool retryable) : std::runtime_error(message), retryable(retryable) {}
	bool retryable;
};

// Cloud-Modelle antworten auf ein Bild normalerweise in wenigen Sekunden; lokale Modelle brauchen
// (vor allem beim ersten Laden) deutlich länger.
long timeoutFor(Provider provider)
{
	switch (provider) {
	case Provider::Gemini:
		return 15; // Antworten kommen in wenigen Sekunden – länger heißt fast immer „hängt“
	case Provider::OpenRouter:
		return 25;
	case Provider::Claude:
		return 45;
	default:
		return 120;
	}
}

// Claude-Modelle, bei denen Anthropic eine Ablehnung serverseitig mit einem anderen Modell wiederholen kann
const std::set<std::string> kClaudeFallbackModels = {"claude-fable-5-1", "claude-opus-5-5", "claude-opus-5",
						     "claude-sonnet-5-5"};

QJsonObject detectSchema()
{
	return QJsonObject{
		{"type", "object"},
		{"properties", QJsonObject{{"is_game", QJsonObject{{"type", "boolean"}}},
					   {"game", QJsonObject{{"type", "string"}}},
					   {"twitch_category", QJsonObject{{"type", "string"}}},
					   {"confidence", QJsonObject{{"type", "number"}}},
					   {"reason", QJsonObject{{"type", "string"}}}}},
		{"required", QJsonArray{"is_game", "game", "twitch_category", "confidence", "reason"}},
		{"additionalProperties", false},
	};
}

QJsonObject titleSchema()
{
	return QJsonObject{
		{"type", "object"},
		{"properties", QJsonObject{{"title", QJsonObject{{"type", "string"}}}}},
		{"required", QJsonArray{"title"}},
		{"additionalProperties", false},
	};
}

std::string trimSlash(std::string url)
{
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	return url;
}

std::string toJson(const QJsonObject &object)
{
	return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

QJsonDocument parseJson(const std::string &text)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(text));
}

std::string providerErrorDetail(const std::string &body)
{
	QJsonDocument doc = parseJson(body);
	// Gemini liefert Fehler teils als Array
	QJsonObject object = doc.isArray() && !doc.array().isEmpty() ? doc.array().first().toObject() : doc.object();
	QJsonValue error = object.value("error");
	if (error.isObject())
		return error.toObject().value("message").toString().toStdString();
	if (error.isString())
		return error.toString().toStdString();
	return object.value("message").toString().toStdString();
}

[[noreturn]] void throwHttpError(const AiTarget &target, const HttpResponse &response)
{
	if (response.timedOut)
		throw AiError("„" + target.model + "“ antwortet nicht (Zeitüberschreitung nach " +
				      std::to_string(timeoutFor(target.provider)) + " s)",
			      true);
	if (!response.error.empty()) {
		if (target.provider == Provider::Ollama)
			throw AiError("Ollama nicht erreichbar – läuft Ollama? (" + response.error + ")", false);
		throw AiError("Keine Verbindung zum KI-Anbieter: " + response.error, true);
	}

	std::string message;
	bool retryable = false;
	switch (response.status) {
	case 400:
		message = "Anfrage vom KI-Anbieter abgelehnt (HTTP 400)";
		break;
	case 401:
	case 403:
		message = "API-Key ungültig oder ohne Berechtigung";
		break;
	case 404:
		message = "Modell „" + target.model + "“ nicht gefunden – in den Einstellungen „Modelle laden“";
		break;
	case 429:
		// Bei Gemini hat jedes Modell ein eigenes Gratis-Kontingent – das Ersatzmodell kann helfen
		message = "Limit für „" + target.model + "“ erreicht (zu viele Anfragen)";
		retryable = true;
		break;
	case 503:
		message = "„" + target.model + "“ ist gerade überlastet (HTTP 503)";
		retryable = true;
		break;
	default:
		retryable = response.status >= 500;
		message = (retryable ? "Server des KI-Anbieters hat ein Problem (HTTP "
				     : "Fehler vom KI-Anbieter (HTTP ") +
			  std::to_string(response.status) + ")";
	}
	const std::string detail = providerErrorDetail(response.body);
	if (!detail.empty() && response.status != 503)
		message += ": " + QString::fromStdString(detail).left(200).toStdString();
	throw AiError(message, retryable);
}

// Schwächere Modelle verpacken JSON gern in ```json … ``` oder schreiben Text drumherum
QJsonObject extractJsonObject(const std::string &text)
{
	const size_t start = text.find('{');
	const size_t end = text.rfind('}');
	if (start != std::string::npos && end != std::string::npos && end > start) {
		QJsonDocument doc = parseJson(text.substr(start, end - start + 1));
		if (doc.isObject())
			return doc.object();
	}
	throw std::runtime_error("Antwort der KI nicht verstanden: " +
				 QString::fromStdString(text).left(120).simplified().toStdString());
}

std::string askOpenAiCompatible(const AiTarget &target, const std::string &system, const QJsonArray &userContent)
{
	if (target.baseUrl.empty())
		throw std::runtime_error("Keine Server-URL für den KI-Anbieter eingetragen");

	QJsonObject request{
		{"model", QString::fromStdString(target.model)},
		{"messages", QJsonArray{QJsonObject{{"role", "system"}, {"content", QString::fromStdString(system)}},
					QJsonObject{{"role", "user"}, {"content", userContent}}}},
		{"max_tokens", 2048},
		{"temperature", 0.2},
	};
	// Niedrige Denkstufe: schnell, aber genau genug für die Bilderkennung
	if (target.provider == Provider::Gemini)
		request["reasoning_effort"] = "low";

	std::vector<std::string> headers{"Content-Type: application/json"};
	if (!target.apiKey.empty())
		headers.push_back("Authorization: Bearer " + target.apiKey);
	if (target.provider == Provider::OpenRouter)
		headers.push_back("X-Title: KI Game Switcher");

	const std::string url = trimSlash(target.baseUrl) + "/chat/completions";
	HttpResponse response = httpRequest("POST", url, headers, toJson(request), timeoutFor(target.provider));
	// Nicht jedes Modell unterstützt jede Denkstufe – dann ohne Vorgabe erneut
	if (response.status == 400 && request.contains("reasoning_effort") &&
	    QString::fromStdString(response.body).contains("thinking", Qt::CaseInsensitive)) {
		request.remove("reasoning_effort");
		response = httpRequest("POST", url, headers, toJson(request), timeoutFor(target.provider));
	}
	if (response.status != 200)
		throwHttpError(target, response);

	QJsonArray choices = parseJson(response.body).object().value("choices").toArray();
	if (choices.isEmpty())
		throw std::runtime_error("Leere Antwort der KI");
	QJsonObject choice = choices.first().toObject();
	QJsonValue content = choice.value("message").toObject().value("content");

	std::string text;
	if (content.isString()) {
		text = content.toString().toStdString();
	} else if (content.isArray()) {
		for (const QJsonValue &part : content.toArray())
			text += part.toObject().value("text").toString().toStdString();
	}
	if (text.empty()) {
		if (choice.value("finish_reason").toString() == "length")
			throw std::runtime_error("Antwort der KI wurde abgeschnitten");
		throw std::runtime_error("Leere Antwort der KI");
	}
	return text;
}

std::string askClaude(const AiTarget &target, const std::string &system, const QJsonArray &content,
		      const QJsonObject &schema)
{
	QJsonObject request{
		{"model", QString::fromStdString(target.model)},
		{"max_tokens", 2048},
		{"system", QString::fromStdString(system)},
		{"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", content}}}},
		{"output_config",
		 QJsonObject{{"effort", "low"}, {"format", QJsonObject{{"type", "json_schema"}, {"schema", schema}}}}},
	};
	std::vector<std::string> headers{"Content-Type: application/json", "anthropic-version: 2023-06-01",
					 "x-api-key: " + target.apiKey};
	if (kClaudeFallbackModels.count(target.model)) {
		headers.push_back("anthropic-beta: server-side-fallback-2026-07-01");
		request["fallbacks"] = "default";
	}

	HttpResponse response = httpRequest("POST", trimSlash(target.baseUrl) + "/v1/messages", headers,
					    toJson(request), timeoutFor(target.provider));
	if (response.status != 200)
		throwHttpError(target, response);

	QJsonObject message = parseJson(response.body).object();
	const QString stopReason = message.value("stop_reason").toString();
	if (stopReason == "refusal") {
		QString category = message.value("stop_details").toObject().value("category").toString();
		throw std::runtime_error("Claude hat die Anfrage abgelehnt (" +
					 (category.isEmpty() ? std::string("ohne Angabe") : category.toStdString()) +
					 ")");
	}
	if (stopReason == "max_tokens")
		throw std::runtime_error("Antwort von Claude wurde abgeschnitten");
	for (const QJsonValue &block : message.value("content").toArray()) {
		if (block.toObject().value("type").toString() == "text")
			return block.toObject().value("text").toString().toStdString();
	}
	throw std::runtime_error("Keine Textantwort von Claude erhalten");
}

QJsonObject askForJson(const AiTarget &target, const char *system, const char *jsonFormat, const QJsonObject &schema,
		       const std::string &userText, const EncodedImage *image)
{
	if (Provider::Claude == target.provider) {
		QJsonArray content;
		if (image) {
			content.append(QJsonObject{
				{"type", "image"},
				{"source", QJsonObject{{"type", "base64"},
						       {"media_type", image->mimeType},
						       {"data", QString::fromLatin1(image->data.toBase64())}}}});
		}
		content.append(QJsonObject{{"type", "text"}, {"text", QString::fromStdString(userText)}});
		return extractJsonObject(askClaude(target, system, content, schema));
	}

	QJsonArray content{QJsonObject{{"type", "text"}, {"text", QString::fromStdString(userText)}}};
	if (image) {
		const QString url =
			QString("data:%1;base64,%2")
				.arg(QString::fromLatin1(image->mimeType), QString::fromLatin1(image->data.toBase64()));
		content.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", url}}}});
	}
	const std::string fullSystem = std::string(system) + "\n\n" + jsonFormat;
	return extractJsonObject(askOpenAiCompatible(target, fullSystem, content));
}

// Modelle, die gerade überlastet waren, werden eine Weile erst nach dem anderen Modell gefragt
constexpr auto kCooldown = std::chrono::minutes(10);
std::mutex g_cooldownMutex;
std::map<std::string, std::chrono::steady_clock::time_point> g_cooldownUntil;

bool coolingDown(const std::string &model)
{
	std::lock_guard lock(g_cooldownMutex);
	auto it = g_cooldownUntil.find(model);
	return it != g_cooldownUntil.end() && std::chrono::steady_clock::now() < it->second;
}

void setCooldown(const std::string &model, bool active)
{
	std::lock_guard lock(g_cooldownMutex);
	if (active)
		g_cooldownUntil[model] = std::chrono::steady_clock::now() + kCooldown;
	else
		g_cooldownUntil.erase(model);
}

// Probiert bei Überlastung, Limit oder Zeitüberschreitung das jeweils andere Modell
template<typename Ask> auto withFallback(const AiTarget &target, Ask ask)
{
	if (target.fallbackModel.empty() || target.fallbackModel == target.model)
		return ask(target);

	AiTarget first = target;
	AiTarget second = target;
	second.model = target.fallbackModel;
	if (coolingDown(target.model) && !coolingDown(target.fallbackModel))
		std::swap(first, second);

	try {
		auto result = ask(first);
		setCooldown(first.model, false);
		return result;
	} catch (const AiError &error) {
		if (!error.retryable)
			throw;
		setCooldown(first.model, true);
		try {
			auto result = ask(second);
			setCooldown(second.model, false);
			return result;
		} catch (const AiError &secondError) {
			if (secondError.retryable)
				setCooldown(second.model, true);
			throw std::runtime_error(std::string(error.what()) + " – " + secondError.what());
		} catch (const std::exception &secondError) {
			throw std::runtime_error(std::string(error.what()) + " – " + secondError.what());
		}
	}
}

bool toBool(const QJsonValue &value)
{
	if (value.isBool())
		return value.toBool();
	return value.toString().trimmed().compare("true", Qt::CaseInsensitive) == 0;
}

double toConfidence(const QJsonValue &value)
{
	double confidence = value.isDouble() ? value.toDouble() : value.toString().toDouble();
	if (confidence > 1.0) // manche Modelle antworten in Prozent
		confidence /= 100.0;
	return std::clamp(confidence, 0.0, 1.0);
}

} // namespace

AiTarget aiTargetFrom(const Settings &settings)
{
	AiTarget target;
	target.provider = settings.provider;
	target.baseUrl = settings.activeBaseUrl();
	target.apiKey = settings.activeProvider().apiKey;
	target.model = settings.activeModel();
	target.fallbackModel = settings.activeProvider().fallbackModel;
	return target;
}

Detection detectGame(const AiTarget &target, const EncodedImage &image, const std::string &windowHint)
{
	if (providerInfo(target.provider).needsKey && target.apiKey.empty())
		throw std::runtime_error("Kein API-Key für den KI-Anbieter eingetragen (Einstellungen → KI)");
	if (target.model.empty())
		throw std::runtime_error("Kein KI-Modell ausgewählt (Einstellungen → KI)");

	std::string text = "Which game is being played in this stream screenshot?";
	if (!windowHint.empty())
		text = windowHint + "\n" + text;

	return withFallback(target, [&](const AiTarget &attempt) {
		QJsonObject reply = askForJson(attempt, kDetectSystem, kDetectJsonFormat, detectSchema(), text, &image);
		Detection detection;
		detection.isGame = toBool(reply.value("is_game"));
		detection.game = reply.value("game").toString().trimmed().toStdString();
		detection.category = reply.value("twitch_category").toString().trimmed().toStdString();
		detection.confidence = toConfidence(reply.value("confidence"));
		detection.reason = reply.value("reason").toString().trimmed().toStdString();
		detection.model = attempt.model;
		if (detection.isGame && detection.category.empty())
			detection.category = detection.game;
		return detection;
	});
}

std::string writeTitle(const AiTarget &target, const std::string &category, const std::string &style,
		       const std::string &previousTitle)
{
	const std::string text =
		"New game / Twitch category: " + category +
		"\nStreamer's style instructions: " + (style.empty() ? "-" : style) +
		"\nPrevious title (tone reference only): " + (previousTitle.empty() ? "-" : previousTitle);
	QJsonObject reply = withFallback(target, [&](const AiTarget &attempt) {
		return askForJson(attempt, kTitleSystem, kTitleJsonFormat, titleSchema(), text, nullptr);
	});

	QString title = reply.value("title").toString().simplified();
	while (!title.isEmpty() && QString("\"'„“”").contains(title.front()))
		title.remove(0, 1);
	while (!title.isEmpty() && QString("\"'„“”").contains(title.back()))
		title.chop(1);
	return title.left(140).toStdString();
}

std::vector<std::string> listModels(const AiTarget &target)
{
	std::vector<std::string> models;
	HttpResponse response;
	if (target.provider == Provider::Claude) {
		response = httpRequest("GET", trimSlash(target.baseUrl) + "/v1/models?limit=100",
				       {"anthropic-version: 2023-06-01", "x-api-key: " + target.apiKey});
	} else {
		std::vector<std::string> headers;
		if (!target.apiKey.empty())
			headers.push_back("Authorization: Bearer " + target.apiKey);
		response = httpRequest("GET", trimSlash(target.baseUrl) + "/models", headers);
	}
	if (response.status != 200)
		throwHttpError(target, response);

	for (const QJsonValue &entry : parseJson(response.body).object().value("data").toArray()) {
		QJsonObject model = entry.toObject();
		QString id = model.value("id").toString();
		if (id.startsWith("models/"))
			id = id.mid(7);
		if (id.isEmpty())
			continue;

		if (target.provider == Provider::Gemini) {
			// Nur Gemini-Chatmodelle, keine Embedding-/Bild-/Audio-Generatoren
			static const char *excluded[] = {"embedding", "image", "tts", "audio", "live", "veo", "imagen"};
			if (!id.startsWith("gemini") ||
			    std::any_of(std::begin(excluded), std::end(excluded),
					[&id](const char *word) { return id.contains(word); }))
				continue;
		} else if (target.provider == Provider::OpenRouter) {
			// Nur kostenlose Modelle, die Bilder verstehen
			QJsonObject pricing = model.value("pricing").toObject();
			QJsonArray modalities =
				model.value("architecture").toObject().value("input_modalities").toArray();
			const bool free = id.endsWith(":free") || (pricing.value("prompt").toString() == "0" &&
								   pricing.value("completion").toString() == "0");
			if (!free || !modalities.contains("image"))
				continue;
		}
		models.push_back(id.toStdString());
	}

	std::sort(models.begin(), models.end());
	if (target.provider == Provider::OpenRouter &&
	    std::find(models.begin(), models.end(), "openrouter/free") == models.end())
		models.insert(models.begin(), "openrouter/free");
	return models;
}

} // namespace kgs
