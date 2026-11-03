#!/usr/bin/env bash
# FFmpeg's -dev headers without sudo: download the .deb files and unpack them
# into third_party/ffmpeg (gitignored).  The runtime .so files come from the
# system (libavcodec.so.60 & co); CMake links them by full path.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
(cd "$tmp" && apt-get download libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libva-dev)
mkdir -p third_party/ffmpeg
for d in "$tmp"/*.deb; do dpkg -x "$d" third_party/ffmpeg; done
echo "headers in third_party/ffmpeg/usr/include"
