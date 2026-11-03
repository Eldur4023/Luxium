#!/usr/bin/env bash
# Format-matrix fixtures (short clips, system ffmpeg) into examples/media/fixtures/matrix:
# one file per container/codec family, with audio on a 440/660 Hz tone so tracks can be told apart.
# Used by tools/matrixtest.py and docs/MEDIA.md.
set -euo pipefail
out="$(dirname "$0")/../examples/media/fixtures/matrix"
mkdir -p "$out"
cd "$out"
F="ffmpeg -loglevel error -y"
V="-f lavfi -i testsrc2=size=640x360:rate=25"
A1="-f lavfi -i sine=frequency=440:sample_rate=48000"
A2="-f lavfi -i sine=frequency=660:sample_rate=44100"
T="-t 6"
python3 - <<'PY'
srt = "".join("%d\n00:00:%02d,000 --> 00:00:%02d,800\nSRT line %d\n\n" % (i + 1, i + 1, i + 1, i + 1) for i in range(4))
open("a.srt", "w").write(srt)
ass = ["[Script Info]", "ScriptType: v4.00+", "PlayResX: 640", "PlayResY: 360", "", "[V4+ Styles]",
       "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,MarginL,MarginR,MarginV,Encoding",
       "Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1", "",
       "[Events]", "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text"]
for i in range(4):
    ass.append("Dialogue: 0,0:00:%02d.00,0:00:%02d.80,Default,,0,0,0,,{\\i1}ASS{\\i0} line %d\\NSecond row" % (i + 1, i + 1, i + 1))
open("a.ass", "w").write("\n".join(ass) + "\n")
PY
# MKV: HEVC 10-bit, AC-3 5.1 + AAC stereo, SRT + ASS
$F $V $A1 $A2 -i a.srt -i a.ass $T -map 0:v -map 1:a -map 2:a -map 3 -map 4 \
   -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -x265-params log-level=error -g 25 \
   -c:a:0 ac3 -ac:a:0 6 -c:a:1 aac -c:s:0 srt -c:s:1 ass \
   -metadata:s:a:0 language=eng -metadata:s:a:1 language=spa -metadata:s:s:0 language=eng -metadata:s:s:1 language=spa \
   -disposition:a:0 default hevc10_ac3_aac_subs.mkv &
$F $V $A1 $T -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a dca -strict -2 h264_dts.mkv &
$F $V $A1 $T -c:v mpeg4 -vtag XVID -c:a libmp3lame xvid_mp3.avi &
$F $V $A1 $T -c:v prores_ks -profile:v 2 -c:a pcm_s16le prores_pcm.mov &
$F $V $A1 $T -c:v wmv2 -c:a wmav2 wmv2_wma.wmv &
$F -f lavfi -i testsrc2=size=720x576:rate=25 $A1 $T -vf "tinterlace=mode=interleave_top" -c:v mpeg2video -flags +ildct+ilme -top 1 -b:v 4M -c:a mp2 mpeg2_interlaced_mp2.ts &
$F $V $A1 $T -c:v flv -c:a libmp3lame flv1_mp3.flv &
$F $V $A1 $T -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac h264_aac_flv.flv &
$F $V $A1 $T -c:v libsvtav1 -preset 12 -pix_fmt yuv420p -c:a libopus av1_opus.webm &
$F $V $A1 $T -c:v libx264 -preset veryfast -profile:v high10 -pix_fmt yuv420p10le -c:a eac3 h264_hi10p_eac3.mp4 &
wait
$F $V $A1 $T -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a flac h264_flac.mkv &
$F $V $A1 $T -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a truehd -strict -2 -ac 2 h264_truehd.mkv &
$F $V $A1 $T -c:v mpeg4 -c:a aac mpeg4_aac.mp4 &
$F $V $A1 $T -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -c:a libvorbis vp9_vorbis.webm &
$F $V $A1 $T -c:v libx264 -preset veryfast -pix_fmt yuv444p -c:a aac h264_444_aac.mkv &
wait
echo "matrix fixtures in $out"
