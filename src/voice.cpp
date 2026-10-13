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

#include "voice.hpp"

#include "http.hpp"

// obs-module.h muss vor plugin-support.h stehen, sonst kollidiert die blogva-Deklaration
#include <obs-module.h>
#include <plugin-support.h>

#include <QString>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef KGS_VOICE
#include <whisper.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef PF_AVX2_INSTRUCTIONS_AVAILABLE
#define PF_AVX2_INSTRUCTIONS_AVAILABLE 40
#endif
#endif

namespace kgs::voice {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRate = 16000;           // whisper erwartet 16 kHz mono
constexpr size_t kBufferSeconds = 120; // so viel Audio wird höchstens vorgehalten
[[maybe_unused]] constexpr size_t kMaxListenSeconds = 60;
constexpr auto kTextWindow = std::chrono::minutes(3);
constexpr auto kRetryDownloadAfter = std::chrono::minutes(5);
constexpr const char *kModelBaseUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";

// -- Audio: wird im Audio-Thread von OBS gefüllt. Nur audioMutex sperren – nie zusammen mit configMutex,
// denn OBS hält beim Aufruf des Callbacks seine eigene Sperre (sonst Verklemmung beim Abmelden).
std::mutex g_audioMutex;
std::vector<float> g_ring(kRate *kBufferSeconds);
size_t g_head = 0;    // nächste Schreibposition
size_t g_pending = 0; // noch nicht umgewandelte Samples
double g_phase = 0.0; // Fortschritt beim Herunterrechnen auf 16 kHz
double g_sum = 0.0;
int g_count = 0;

// -- Konfiguration und Quelle
std::mutex g_configMutex;
bool g_enabled = false;
std::string g_sourceSetting;
std::string g_modelId = "base";
obs_source_t *g_source = nullptr;
std::string g_sourceName;
bool g_sourceMissing = false;

// -- Download des Sprachmodells
std::thread g_download;
std::atomic<bool> g_downloading{false};
std::atomic<int> g_progress{-1};
std::string g_downloadError; // geschützt durch g_configMutex
Clock::time_point g_downloadFailedAt{};

// -- Erkannter Text
std::mutex g_textMutex;
std::deque<std::pair<Clock::time_point, std::string>> g_texts;
std::string g_lastText;

std::string modelPath(const std::string &id)
{
	return configPath((std::string("models/") + voiceModel(id).file).c_str());
}

bool fileExists(const std::string &path)
{
	std::error_code error;
	return std::filesystem::exists(std::filesystem::u8path(path), error);
}

bool isMicrophone(obs_source_t *source)
{
	if (!source || !(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO))
		return false;
	const char *id = obs_source_get_unversioned_id(source);
	const std::string type = id ? id : "";
	return type.find("input_capture") != std::string::npos || type.find("jack") != std::string::npos;
}

void pushSample(float sample, double step)
{
	g_sum += sample;
	g_count++;
	g_phase += 1.0;
	if (g_phase < step)
		return;
	g_phase -= step;
	g_ring[g_head] = static_cast<float>(g_sum / g_count);
	g_head = (g_head + 1) % g_ring.size();
	g_pending = std::min(g_pending + 1, g_ring.size());
	g_sum = 0.0;
	g_count = 0;
}

void audioCallback(void *, obs_source_t *, const struct audio_data *audio, bool muted)
{
	audio_t *output = obs_get_audio();
	if (!output || !audio || audio->frames == 0)
		return;
	const size_t channels = std::max<size_t>(1, audio_output_get_channels(output));
	const double step = audio_output_get_sample_rate(output) / static_cast<double>(kRate);

	std::lock_guard lock(g_audioMutex);
	for (uint32_t frame = 0; frame < audio->frames; frame++) {
		float mixed = 0.0f;
		if (!muted) {
			size_t used = 0;
			for (size_t channel = 0; channel < channels && channel < MAX_AV_PLANES; channel++) {
				if (!audio->data[channel])
					continue;
				mixed += reinterpret_cast<const float *>(audio->data[channel])[frame];
				used++;
			}
			if (used)
				mixed /= static_cast<float>(used);
		}
		pushSample(mixed, step);
	}
}

void clearAudio()
{
	std::lock_guard lock(g_audioMutex);
	g_pending = 0;
	g_phase = 0.0;
	g_sum = 0.0;
	g_count = 0;
}

// Erwartet g_configMutex
void detachLocked()
{
	if (!g_source)
		return;
	obs_source_remove_audio_capture_callback(g_source, audioCallback, nullptr);
	obs_source_release(g_source);
	g_source = nullptr;
	g_sourceName.clear();
	clearAudio();
}

obs_source_t *findSource(const std::string &name)
{
	if (!name.empty())
		return obs_get_source_by_name(name.c_str());
	const std::vector<std::string> sources = audioSources();
	return sources.empty() ? nullptr : obs_get_source_by_name(sources.front().c_str());
}

// Erwartet g_configMutex
void attachLocked()
{
	if (g_source && !obs_source_removed(g_source) && (g_sourceSetting.empty() || g_sourceSetting == g_sourceName))
		return;
	detachLocked();
	obs_source_t *source = findSource(g_sourceSetting);
	g_sourceMissing = !source;
	if (!source)
		return;
	obs_source_add_audio_capture_callback(source, audioCallback, nullptr);
	g_source = source;
	g_sourceName = obs_source_get_name(source);
	obs_log(LOG_INFO, "Spracherkennung hört bei „%s“ mit", g_sourceName.c_str());
}

// Erwartet g_configMutex
void startDownloadLocked()
{
	const std::string id = g_modelId;
	const std::string path = modelPath(id);
	if (fileExists(path) || g_downloading)
		return;
	if (!g_downloadError.empty() && Clock::now() - g_downloadFailedAt < kRetryDownloadAfter)
		return;
	if (g_download.joinable())
		g_download.join();

	g_downloading = true;
	g_progress = -1;
	g_downloadError.clear();
	const std::string url = std::string(kModelBaseUrl) + voiceModel(id).file;
	obs_log(LOG_INFO, "Lade Sprachmodell %s", url.c_str());
	g_download = std::thread([url, path] {
		const std::string error = httpDownload(url, path, [](int percent) { g_progress = percent; });
		{
			std::lock_guard lock(g_configMutex);
			g_downloadError = error;
			if (!error.empty()) {
				g_downloadFailedAt = Clock::now();
				obs_log(LOG_WARNING, "Sprachmodell nicht geladen: %s", error.c_str());
			}
		}
		g_downloading = false;
	});
}

QString recentText()
{
	std::lock_guard lock(g_textMutex);
	const Clock::time_point cutoff = Clock::now() - kTextWindow;
	while (!g_texts.empty() && g_texts.front().first < cutoff)
		g_texts.pop_front();
	QStringList parts;
	for (const auto &entry : g_texts)
		parts << QString::fromStdString(entry.second);
	QString text = parts.join(' ').simplified();
	if (text.size() > 600)
		text = QStringLiteral("…") + text.right(600);
	return text;
}

#ifdef KGS_VOICE

std::mutex g_whisperMutex;
whisper_context *g_whisper = nullptr;
std::string g_whisperModel;

void silentLog(enum ggml_log_level level, const char *text, void *)
{
	if (level >= GGML_LOG_LEVEL_ERROR && text)
		obs_log(LOG_WARNING, "whisper: %s", text);
}

// Liest das Modell über einen Datei-Stream mit Unicode-Pfad (whisper selbst kann unter Windows keine Umlaute)
struct FileLoader {
	std::ifstream stream;
};

whisper_context *loadModel(const std::string &path)
{
	auto *file = new FileLoader;
	file->stream.open(std::filesystem::u8path(path), std::ios::binary);
	if (!file->stream) {
		delete file;
		return nullptr;
	}
	whisper_model_loader loader{};
	loader.context = file;
	loader.read = [](void *ctx, void *output, size_t size) -> size_t {
		auto *f = static_cast<FileLoader *>(ctx);
		f->stream.read(static_cast<char *>(output), static_cast<std::streamsize>(size));
		return static_cast<size_t>(f->stream.gcount());
	};
	loader.eof = [](void *ctx) {
		return static_cast<FileLoader *>(ctx)->stream.eof();
	};
	loader.close = [](void *ctx) {
		delete static_cast<FileLoader *>(ctx);
	};

	whisper_context_params params = whisper_context_default_params();
	params.use_gpu = false; // die Grafikkarte gehört dem Spiel und der Bild-KI
	return whisper_init_with_params(&loader, params);
}

// Typische Whisper-Halluzinationen bei Stille oder Musik
bool isHallucination(const QString &text)
{
	static const QStringList phrases = {
		"Untertitel", "Amara.org",     "Vielen Dank fürs Zuschauen", "Vielen Dank für's Zuschauen",
		"Copyright",  "Untertitelung", "Thanks for watching",        "Bis zum nächsten Mal"};
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return text.size() < 3;
}

#endif

} // namespace

bool supported(std::string *reason)
{
#ifndef KGS_VOICE
	if (reason)
		*reason = "Auf diesem System noch nicht verfügbar";
	return false;
#else
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	if (!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) {
		if (reason)
			*reason = "Der Prozessor unterstützt kein AVX2 – Spracherkennung nicht möglich";
		return false;
	}
#elif defined(__x86_64__)
	if (!__builtin_cpu_supports("avx2")) {
		if (reason)
			*reason = "Der Prozessor unterstützt kein AVX2 – Spracherkennung nicht möglich";
		return false;
	}
#endif
	return true;
#endif
}

void configure(const Settings &settings)
{
	const bool enabled = settings.voiceEnabled && supported();
	std::lock_guard lock(g_configMutex);
	g_enabled = enabled;
	g_sourceSetting = settings.voiceSource;
	g_modelId = voiceModel(settings.voiceModel).id;
	if (!enabled) {
		detachLocked();
		return;
	}
	attachLocked();
	startDownloadLocked();
}

std::string listen(const std::string &hint)
{
#ifndef KGS_VOICE
	(void)hint;
	return {};
#else
	std::string modelId;
	{
		std::lock_guard lock(g_configMutex);
		if (!g_enabled || g_downloading)
			return recentText().toStdString();
		modelId = g_modelId;
	}

	std::vector<float> samples;
	{
		std::lock_guard lock(g_audioMutex);
		const size_t count = std::min(g_pending, kMaxListenSeconds * kRate);
		samples.resize(count);
		const size_t start = (g_head + g_ring.size() - count) % g_ring.size();
		for (size_t i = 0; i < count; i++)
			samples[i] = g_ring[(start + i) % g_ring.size()];
		g_pending = 0;
	}
	if (samples.size() < static_cast<size_t>(kRate))
		return recentText().toStdString();

	double energy = 0.0;
	for (float sample : samples)
		energy += static_cast<double>(sample) * sample;
	if (std::sqrt(energy / samples.size()) < 0.003) // Stille: nichts zu tun
		return recentText().toStdString();

	std::lock_guard lock(g_whisperMutex);
	if (!g_whisper || g_whisperModel != modelId) {
		if (g_whisper)
			whisper_free(g_whisper);
		g_whisper = nullptr;
		const std::string path = modelPath(modelId);
		if (!fileExists(path))
			return recentText().toStdString();
		whisper_log_set(silentLog, nullptr);
		g_whisper = loadModel(path);
		g_whisperModel = modelId;
		if (!g_whisper) {
			// Kaputte Datei (z. B. abgebrochener Download) löschen, damit sie neu geladen wird
			std::error_code ignored;
			std::filesystem::remove(std::filesystem::u8path(path), ignored);
			std::lock_guard config(g_configMutex);
			g_downloadError = "Sprachmodell beschädigt – wird neu geladen";
			return recentText().toStdString();
		}
	}

	const std::string prompt = hint.empty() ? std::string() : "Spiel: " + hint + ".";
	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.language = "de";
	params.translate = false;
	params.n_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);
	params.no_context = true;
	params.no_timestamps = true;
	params.print_progress = false;
	params.print_realtime = false;
	params.print_special = false;
	params.print_timestamps = false;
	params.suppress_blank = true;
	params.suppress_nst = true;
	params.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();

	if (whisper_full(g_whisper, params, samples.data(), static_cast<int>(samples.size())) != 0) {
		obs_log(LOG_WARNING, "Spracherkennung fehlgeschlagen");
		return recentText().toStdString();
	}

	QStringList sentences;
	const int segments = whisper_full_n_segments(g_whisper);
	for (int i = 0; i < segments; i++) {
		if (whisper_full_get_segment_no_speech_prob(g_whisper, i) > 0.6f)
			continue;
		const QString text = QString::fromUtf8(whisper_full_get_segment_text(g_whisper, i)).simplified();
		if (!isHallucination(text))
			sentences << text;
	}
	if (!sentences.isEmpty()) {
		const std::string text = sentences.join(' ').toStdString();
		std::lock_guard textLock(g_textMutex);
		g_texts.emplace_back(Clock::now(), text);
		g_lastText = text;
	}
	return recentText().toStdString();
#endif
}

std::string status()
{
	std::string reason;
	if (!supported(&reason))
		return reason;
	std::lock_guard lock(g_configMutex);
	if (!g_enabled)
		return "Aus";
	if (g_downloading) {
		const int percent = g_progress;
		return percent >= 0 ? "Sprachmodell wird geladen … " + std::to_string(percent) + " %"
				    : std::string("Sprachmodell wird geladen …");
	}
	if (!g_downloadError.empty())
		return "Sprachmodell nicht geladen: " + g_downloadError;
	if (!g_source)
		return g_sourceMissing ? "Kein Mikrofon gefunden – Quelle in den Einstellungen wählen"
				       : "Wartet auf das Mikrofon";
	return "Hört zu: " + g_sourceName;
}

std::string lastText()
{
	std::lock_guard lock(g_textMutex);
	return g_lastText;
}

std::vector<std::string> audioSources()
{
	std::vector<std::string> names;
	auto add = [&names](obs_source_t *source) {
		if (!isMicrophone(source))
			return;
		const char *name = obs_source_get_name(source);
		if (name && *name && std::find(names.begin(), names.end(), name) == names.end())
			names.emplace_back(name);
	};
	// Globale Audiogeräte aus den OBS-Einstellungen (z. B. „Mic/Aux“) zuerst
	for (uint32_t channel = 1; channel <= 6; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		add(source);
		obs_source_release(source);
	}
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			(*static_cast<decltype(add) *>(param))(source);
			return true;
		},
		&add);
	return names;
}

void shutdown()
{
	{
		std::lock_guard lock(g_configMutex);
		g_enabled = false;
		detachLocked();
	}
	if (g_download.joinable())
		g_download.join();
#ifdef KGS_VOICE
	std::lock_guard lock(g_whisperMutex);
	if (g_whisper)
		whisper_free(g_whisper);
	g_whisper = nullptr;
#endif
}

} // namespace kgs::voice
