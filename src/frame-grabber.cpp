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

#include "frame-grabber.hpp"
#include "i18n.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/vec4.h>

#include <QBuffer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace kgs {

namespace {

struct SourceRef {
	obs_source_t *source;
	~SourceRef() { obs_source_release(source); }
};

} // namespace

// Gleiches Vorgehen wie obs-websocket bei GetSourceScreenshot: in eine Textur in Zielgröße rendern
// (die GPU skaliert dabei) und über eine Staging-Surface in den Speicher kopieren.
QImage grabFrame(const std::string &sourceName, int maxWidth)
{
	obs_source_t *source = nullptr;
	if (!sourceName.empty()) {
		source = obs_get_source_by_name(sourceName.c_str());
		if (!source)
			throw std::runtime_error(Trf("Capture.SourceNotFound", {sourceName}));
	} else {
		source = obs_get_output_source(0);
		if (!source)
			source = obs_frontend_get_current_scene();
		if (!source)
			throw std::runtime_error(Tr("Capture.NoProgramOutputFound"));
	}
	SourceRef ref{source};

	const uint32_t sourceWidth = obs_source_get_width(source);
	const uint32_t sourceHeight = obs_source_get_height(source);
	if (!sourceWidth || !sourceHeight)
		return {};
	const uint32_t width = std::min(static_cast<uint32_t>(std::max(maxWidth, 64)), sourceWidth);
	const uint32_t height = std::max<uint32_t>(
		2, static_cast<uint32_t>(std::lround(static_cast<double>(sourceHeight) * width / sourceWidth)));

	QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGBA8888);
	bool ok = false;

	obs_enter_graphics();
	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stagesurf = gs_stagesurface_create(width, height, GS_RGBA);
	gs_texrender_reset(texrender);
	if (gs_texrender_begin(texrender, width, height)) {
		vec4 background;
		vec4_zero(&background);
		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(sourceWidth), 0.0f, static_cast<float>(sourceHeight), -100.0f,
			 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_inc_showing(source);
		obs_source_video_render(source);
		obs_source_dec_showing(source);
		gs_blend_state_pop();
		gs_texrender_end(texrender);

		gs_stage_texture(stagesurf, gs_texrender_get_texture(texrender));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stagesurf, &data, &linesize)) {
			for (uint32_t y = 0; y < height; y++)
				std::memcpy(image.scanLine(static_cast<int>(y)), data + y * linesize, width * 4);
			gs_stagesurface_unmap(stagesurf);
			ok = true;
		}
	}
	gs_stagesurface_destroy(stagesurf);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();

	if (!ok)
		return {};
	return image.convertToFormat(QImage::Format_RGB888);
}

bool isBlack(const QImage &image)
{
	const int stepX = std::max(1, image.width() / 48);
	const int stepY = std::max(1, image.height() / 27);
	for (int y = 0; y < image.height(); y += stepY) {
		const uchar *line = image.constScanLine(y);
		for (int x = 0; x < image.width(); x += stepX) {
			const uchar *pixel = line + x * 3;
			if (pixel[0] > 12 || pixel[1] > 12 || pixel[2] > 12)
				return false;
		}
	}
	return true;
}

EncodedImage encodeImage(const QImage &image)
{
	EncodedImage result{{}, "image/jpeg"};
	QBuffer buffer(&result.data);
	buffer.open(QIODevice::WriteOnly);
	if (image.save(&buffer, "JPG", 85))
		return result;

	// Fallback, falls das JPEG-Plugin von Qt fehlt
	result.data.clear();
	buffer.seek(0);
	image.save(&buffer, "PNG");
	result.mimeType = "image/png";
	return result;
}

} // namespace kgs
