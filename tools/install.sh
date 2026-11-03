#!/bin/sh
# Installs a built Luxium for the current user, no sudo:
#   ~/.local/opt/luxium/        the binary + Chromium runtime (libcef, paks, locales)
#   ~/.local/bin/luxium         symlink to it (so `luxium` is on PATH, and Lux Desktop finds it)
#
# Usage: tools/install.sh [build-dir]        (default: ./build, after `cmake --build build`)
# Override the destination with LUXIUM_PREFIX=/some/dir.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$here/build}
dest=${LUXIUM_PREFIX:-$HOME/.local/opt/luxium}
bin=${LUXIUM_BIN_DIR:-$HOME/.local/bin}
cef=$here/third_party/cef

[ -x "$build/luxium" ] || { echo "install: $build/luxium not found -- build first (cmake --build build)" >&2; exit 1; }
[ -f "$cef/Release/libcef.so" ] || { echo "install: third_party/cef missing (tools/fetch_cef.sh)" >&2; exit 1; }

mkdir -p "$dest" "$bin"
# Fresh files go to a staging dir, then swap: a running host keeps its old (still-open) files.
stage=$dest.new.$$
rm -rf "$stage"
mkdir -p "$stage"
cp -a "$cef/Release/." "$stage/"
cp -a "$cef/Resources/." "$stage/"
cp "$build/luxium" "$stage/luxium"
cp "$build/libluxium_cef.so" "$stage/libluxium_cef.so"   # Chromium backend, loaded on the first JavaScript page
rm -f "$stage/chrome-sandbox"        # we run with no_sandbox: nothing to make setuid
# libcef ships with symbols (1.4 GB); the runtime does not need them.
command -v strip >/dev/null 2>&1 && strip --strip-unneeded "$stage/libcef.so" 2>/dev/null || true
# Same layout as the build tree: the binary finds libcef via RPATH $ORIGIN and the paks next to itself.
rm -rf "$dest.old"
[ -e "$dest" ] && mv "$dest" "$dest.old"
mv "$stage" "$dest"
rm -rf "$dest.old"
ln -sf "$dest/luxium" "$bin/luxium"
echo "installed: $dest ($(du -sh "$dest" | cut -f1)), $bin/luxium -> $dest/luxium"

# Ask an idle running host to exit so the next app start runs the new binary; one with windows is left alone.
sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/luxium/host.sock"
if [ -S "$sock" ] && command -v python3 >/dev/null 2>&1; then
  r=$(python3 - "$sock" <<'PY' 2>/dev/null || true
import socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(2)
try:
    s.connect(sys.argv[1]); s.sendall(b'{"cmd":"quit"}\n'); d = s.recv(200)
    print("bye" if b"bye" in d else "busy" if b"busy" in d else "old")
except OSError:
    print("none")
PY
)
  case "$r" in
    bye)  echo "note: the idle Luxium host was asked to exit; the next app start uses the new version" ;;
    busy) echo "note: a Luxium host has windows open; it keeps the old version until those apps are closed (apps restart it by themselves when they start with no windows open)" ;;
    old)  echo "note: the running Luxium host predates 'quit'; close its apps or run: kill \$(cat \"$(dirname "$sock")/host.pid\")" ;;
  esac
fi
