#!/bin/bash
# Build DX100Voice on the norns (or any Linux/macOS box with cmake + g++).
# Needs SuperCollider *source* headers, not just a running sclang.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -z "${SC_PATH:-}" ]; then
	if [ -f /usr/local/include/SuperCollider/plugin_interface/SC_PlugIn.h ]; then
		SC_PATH=/usr/local/include/SuperCollider
	elif [ -f /usr/include/SuperCollider/plugin_interface/SC_PlugIn.h ]; then
		SC_PATH=/usr/include/SuperCollider
	else
		SC_PATH="$HOME/supercollider"
	fi
fi
EXT="${EXT:-$HOME/.local/share/SuperCollider/Extensions/dx100}"

if [ ! -f "$SC_PATH/plugin_interface/SC_PlugIn.h" ] \
	&& [ ! -f "$SC_PATH/include/plugin_interface/SC_PlugIn.h" ]; then
	echo "cloning SuperCollider source to $SC_PATH (headers, depth 1)"
	git clone --depth 1 https://github.com/supercollider/supercollider.git "$SC_PATH"
fi

mkdir -p "$DIR/build"
cd "$DIR/build"
cmake -DSC_PATH="$SC_PATH" -DCMAKE_BUILD_TYPE=Release "$DIR"
cmake --build . --config Release

mkdir -p "$EXT"
if [ -f DX100Voice.so ]; then
	cp -f DX100Voice.so "$EXT/DX100Voice.so"
	cp -f DX100Voice.so "$EXT/DX100Voice_scsynth.so"
	echo "installed $EXT/DX100Voice.so"
elif [ -f DX100Voice.scx ]; then
	cp -f DX100Voice.scx "$EXT/"
	echo "installed $EXT/DX100Voice.scx"
else
	echo "build produced no plugin binary" >&2
	exit 1
fi
# scsynth also scans the system plugin dir. A stale copy of DX100Voice
# there SHADOWS the one we just installed, so the server keeps running old
# code and every rebuild looks like it did nothing. Check for it.
SYSPLUG=/usr/local/lib/SuperCollider/plugins
BUILT="$(pwd)/DX100Voice.so"
[ -f "$BUILT" ] || BUILT="$(pwd)/DX100Voice.scx"
shadowed=0
for f in "$SYSPLUG"/DX100Voice.so "$SYSPLUG"/DX100Voice_scsynth.so \
	"$SYSPLUG"/DX100Voice.scx; do
	[ -f "$f" ] || continue
	if ! cmp -s "$BUILT" "$f"; then
		shadowed=1
		echo "WARNING: stale plugin shadows this build: $f" >&2
	fi
done
if [ "$shadowed" = 1 ]; then
	cat >&2 <<'MSG'

  scsynth loads that copy INSTEAD of the one just installed, so your
  changes will not take effect. Overwrite it too:

    sudo cp DX100Voice.so /usr/local/lib/SuperCollider/plugins/DX100Voice.so
    sudo cp DX100Voice.so /usr/local/lib/SuperCollider/plugins/DX100Voice_scsynth.so

  (run from lib/ugens/build), or delete the stale copies.
MSG
fi

# Do not compile with -DSUPERNOVA. scsynth rejects those plugins
# (server_type 1). SYSTEM > RESTART after install.
echo "SYSTEM > RESTART so scsynth loads the plugin"
