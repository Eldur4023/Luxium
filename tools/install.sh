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
