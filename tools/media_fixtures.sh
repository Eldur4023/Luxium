#!/usr/bin/env bash
# Generates the media test fixtures with the system ffmpeg into
# examples/media/fixtures (gitignored): H.264+AAC MP4, HEVC 10-bit + AC-3 MKV,
# VP9+Opus WebM, a 4K HEVC MP4, HLS with TS and fMP4 segments, a WebVTT file.
set -euo pipefail
out="$(dirname "$0")/../examples/media/fixtures"
mkdir -p "$out/hls_ts" "$out/hls_fmp4"
cd "$out"
F="ffmpeg -loglevel error -y"
V1080="-f lavfi -i testsrc2=size=1920x1080:rate=24"
$F $V1080 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 20 -c:v libx264 -preset veryfast -g 48 -pix_fmt yuv420p -c:a aac -b:a 96k h264_aac.mp4 &
$F $V1080 -f lavfi -i sine=frequency=330:sample_rate=48000 -t 20 -c:v libx265 -preset ultrafast -g 48 -pix_fmt yuv420p10le -x265-params log-level=error -c:a ac3 hevc10_ac3.mkv &
$F -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=550 -t 20 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -g 60 -c:a libopus vp9_opus.webm &
$F -f lavfi -i testsrc2=size=3840x2160:rate=24 -f lavfi -i sine=frequency=700:sample_rate=48000 -t 12 -c:v libx265 -preset ultrafast -g 48 -pix_fmt yuv420p -x265-params log-level=error -c:a aac 4k_hevc.mp4 &
wait
$F -i h264_aac.mp4 -c copy -f hls -hls_time 4 -hls_playlist_type vod -hls_segment_filename hls_ts/s%d.ts hls_ts/index.m3u8
$F $V1080 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 20 -c:v libx264 -preset veryfast -g 48 -sc_threshold 0 -pix_fmt yuv420p -c:a aac \
   -f hls -hls_time 4 -hls_playlist_type vod -hls_flags independent_segments -hls_segment_type fmp4 \
   -hls_fmp4_init_filename init.mp4 -hls_segment_filename hls_fmp4/s%d.m4s hls_fmp4/index.m3u8
python3 - <<'PY'
def ts(t): return "%02d:%02d:%06.3f" % (t // 3600, t // 60 % 60, t % 60)
with open("subs.vtt", "w") as f:
    f.write("WEBVTT\n\n")
    for i in range(10):
        f.write("cue%d\n%s --> %s\nLine %d <b>bold</b>\n\n" % (i, ts(i * 2 + 0.5), ts(i * 2 + 1.5), i))
PY
echo "fixtures in $out"
# hls.js for examples/media/hlsjs.html: reuse homeflix's copy when it is around
hj="$(dirname "$0")/../../homeflix/lux-app/public/js/hls.min.js"
[ -f "$hj" ] && cp "$hj" "$(dirname "$0")/../examples/media/hls.min.js" || true
