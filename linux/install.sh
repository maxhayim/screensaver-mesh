#!/bin/sh
# Installs Mesh for the current user's XScreenSaver.
set -eu
cd "$(dirname "$0")"

# The hack directory differs by distribution.
for dir in /usr/libexec/xscreensaver /usr/lib/xscreensaver /usr/local/libexec/xscreensaver; do
  [ -d "$dir" ] && HACKDIR=$dir && break
done
for dir in /usr/share/xscreensaver/config /usr/local/share/xscreensaver/config; do
  [ -d "$dir" ] && CONFDIR=$dir && break
done
: "${HACKDIR:?XScreenSaver not found. Install it first (e.g. sudo apt install xscreensaver).}"
: "${CONFDIR:=/usr/share/xscreensaver/config}"

echo "Installing to $HACKDIR and $CONFDIR (needs sudo)"
sudo install -m 755 screensaver-mesh "$HACKDIR/screensaver-mesh"
sudo install -D -m 644 screensaver-mesh.xml "$CONFDIR/screensaver-mesh.xml"

CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}/screensaver-mesh/config
if [ ! -f "$CONFIG" ]; then
  mkdir -p "$(dirname "$CONFIG")"
  cp config.example "$CONFIG"
  chmod 600 "$CONFIG"
  echo "Created $CONFIG: add your MeshMonitor server and API token there."
fi

if [ -f "$HOME/.xscreensaver" ] && ! grep -q screensaver-mesh "$HOME/.xscreensaver"; then
  echo
  echo "Add this line to the programs: list in ~/.xscreensaver, then pick Mesh in xscreensaver-settings:"
  echo "  screensaver-mesh -root \\n\\"
fi
echo "Done."
