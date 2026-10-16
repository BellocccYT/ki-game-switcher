#!/bin/sh
# TitleSync – Installation für OBS Studio unter Linux (alle Distributionen)
# TitleSync – installer for OBS Studio on Linux (any distribution)
#
#   ./install.sh               installieren / aktualisieren – install / update
#   ./install.sh --uninstall   entfernen – remove
#
# Das Plugin landet im Benutzerordner von OBS, root-Rechte sind nicht nötig.
# The plugin goes into OBS's per-user plugin folder, no root needed.

set -eu

name=titlesync
here=$(cd "$(dirname "$0")" && pwd)
config="${XDG_CONFIG_HOME:-$HOME/.config}"
target="$config/obs-studio/plugins/$name"

say() { printf '%s\n' "$1" "$2"; }

if [ "$(id -u)" = 0 ]; then
	say "Bitte ohne sudo ausführen – das Plugin wird nur für deinen Benutzer installiert." \
		"Please run without sudo – the plugin is installed for your user only."
	exit 1
fi

if pgrep -x obs >/dev/null 2>&1; then
	say "OBS läuft noch. Bitte OBS schließen und das Skript erneut starten." \
		"OBS is still running. Please close OBS and run the script again."
	exit 1
fi

if [ "${1:-}" = "--uninstall" ]; then
	rm -rf "$target"
	say "TitleSync wurde entfernt. Deine Einstellungen bleiben in $config/obs-studio/plugin_config/$name." \
		"TitleSync has been removed. Your settings stay in $config/obs-studio/plugin_config/$name."
	exit 0
fi

if [ ! -f "$here/$name/bin/64bit/$name.so" ]; then
	say "Plugin-Dateien nicht gefunden. Bitte das Archiv vollständig entpacken und install.sh aus dem entpackten Ordner starten." \
		"Plugin files not found. Please extract the whole archive and run install.sh from the extracted folder."
	exit 1
fi

for system_copy in /usr/lib/obs-plugins/$name.so /usr/lib/*/obs-plugins/$name.so /usr/lib64/obs-plugins/$name.so; do
	if [ -f "$system_copy" ]; then
		say "TitleSync ist bereits systemweit installiert ($system_copy, z. B. über das .deb-Paket). Bitte zuerst entfernen (sudo apt remove $name), sonst lädt OBS das Plugin doppelt." \
			"TitleSync is already installed system-wide ($system_copy, e.g. via the .deb package). Please remove it first (sudo apt remove $name), otherwise OBS loads the plugin twice."
		exit 1
	fi
done

rm -rf "$target"
mkdir -p "$target"
cp -R "$here/$name/." "$target/"

say "TitleSync wurde nach $target installiert. OBS starten → Docks → TitleSync." \
	"TitleSync has been installed to $target. Start OBS → Docks → TitleSync."

if ! command -v obs >/dev/null 2>&1 && command -v flatpak >/dev/null 2>&1 &&
	flatpak info com.obsproject.Studio >/dev/null 2>&1; then
	echo
	say "Hinweis: Du nutzt OBS als Flatpak. Dafür ist dieses Paket nicht gedacht – OBS aus der Paketverwaltung deiner Distribution verwenden." \
		"Note: you are using OBS as a Flatpak. This package is not meant for it – use OBS from your distribution's package manager."
fi
