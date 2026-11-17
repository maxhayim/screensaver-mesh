#!/bin/sh
# Installs the Mesh wallpaper for the current user. No sudo needed.
set -eu
cd "$(dirname "$0")"

BIN=$HOME/.local/bin
UNITS=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}/screensaver-mesh/config

# From a release tarball the binary sits next to this script; in a checkout it is in build/.
SRC=mesh-wallpaper
[ -f "$SRC" ] || SRC=../build/mesh-wallpaper
[ -f "$SRC" ] || { echo "Build it first: make -C wayland"; exit 1; }

install -D -m 755 "$SRC" "$BIN/mesh-wallpaper"
install -D -m 644 mesh-wallpaper.service "$UNITS/mesh-wallpaper.service"
echo "Installed $BIN/mesh-wallpaper"

if [ ! -f "$CONFIG" ]; then
  mkdir -p "$(dirname "$CONFIG")"
  cp config.example "$CONFIG"
  chmod 600 "$CONFIG"
  echo "Created $CONFIG: add your MeshMonitor server and API token there."
fi

if command -v systemctl >/dev/null 2>&1 && systemctl --user daemon-reload 2>/dev/null; then
  echo
  echo "Start it now and at every login (if your session reaches graphical-session.target):"
  echo "  systemctl --user enable --now mesh-wallpaper.service"
  echo "Or start it from your compositor's config instead:"
else
  echo
  echo "Start it from your compositor's config:"
fi
echo "  Hyprland:  exec-once = $BIN/mesh-wallpaper"
echo "  Sway:      exec $BIN/mesh-wallpaper"
echo "Done."
