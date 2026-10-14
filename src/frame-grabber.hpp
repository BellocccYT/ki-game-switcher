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

#pragma once

#include <QByteArray>
#include <QImage>

#include <string>

namespace kgs {

// Rendert die Programmausgabe (sourceName leer) oder eine einzelne Quelle verkleinert auf maxWidth.
// Wirft std::runtime_error, wenn die Quelle fehlt; liefert ein Null-QImage, wenn sie gerade kein Bild hat.
QImage grabFrame(const std::string &sourceName, int maxWidth);

bool isBlack(const QImage &image);

struct EncodedImage {
	QByteArray data;
	const char *mimeType;
};
EncodedImage encodeImage(const QImage &image);

} // namespace kgs
