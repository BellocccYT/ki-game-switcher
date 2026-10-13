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

#pragma once

#include <QImage>

#include <optional>
#include <string>
#include <vector>

namespace kgs {

// Referenzbilder eines Spiels aus dem Steam-Shop
struct ReferenceGame {
	std::string name; // Name bei Steam
	std::vector<QImage> screenshots;
};

// Sucht das Spiel bei Steam (ohne Key) und lädt bis zu zwei Screenshots. Ergebnisse werden zwischengespeichert.
// nullopt, wenn Steam das Spiel nicht führt (z. B. Konsolen-Exklusives) oder nicht erreichbar ist.
std::optional<ReferenceGame> steamReference(const std::string &name);

// Die Screenshots eines Spiels nebeneinander in einem Bild (eine Bildanfrage statt mehrerer)
QImage referenceSheet(const ReferenceGame &game);

} // namespace kgs
