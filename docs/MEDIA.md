# Media in Luxium

Stock CEF has no H.264, HEVC, AAC or AC-3, so `<video>`/`<audio>` cannot play
through Chromium. Luxium plays them with its own engine on top of FFmpeg and
bridges it into CEF pages by re-implementing `HTMLMediaElement` in JavaScript.

## Architecture (as built)

```
 page JS  <video>.play() ...                         CEF renderer process (Blink thread)
    |
 src/media/shim.js        patches HTMLMediaElement.prototype, draws frames (WebGL2),
    |                     text tracks, controls, fullscreen
    |  window.__luxiumMedia.*   V8 functions (synchronous, same thread)
 src/cef/media_bridge.cpp one map<id, media::Player> per frame/V8 context
    |
 src/media/player.cpp     media::Player: demux thread, video decode thread,
                          audio decode thread, SDL audio callback (master clock)
```

- **The engine runs inside the CEF renderer process**, not in the browser
  process. A call from the shim is a plain V8 function call: no IPC, no
  serialisation, and frames go straight from the decoder into a JS
  `ArrayBuffer`. Players die with their V8 context (`on_context_released`) or
  when the element is removed (the shim closes it).
- `shim.js` is embedded as a raw string in the binary (`shim_js.inc`, generated
  by CMake) and `Eval`ed into every http(s)/file/data/about:blank frame in
  `OnContextCreated`, before page scripts. It removes `__luxiumMedia` from
  `window` after capturing it, so pages cannot reach the raw object.
- Without FFmpeg headers at build time `src/media/player_stub.cpp` replaces the
  engine (`media::built()` is false) and the shim is not installed, so the
  build never depends on FFmpeg. `LUXIUM_MEDIA` is ON when the headers exist
  (`tools/fetch_ffmpeg_dev.sh`).
- `media::Player` is also used by the own HTML/CSS engine's `<video>`
  (`src/media/own_media.*`) and by `luxium_mediatest`.

### Engine (`media::Player`)

- Demux: libavformat opens any URL it knows (progressive MP4, MKV, WebM, MPEG-TS,
  HLS `.m3u8`, over http(s); `file:` only for local pages). Request headers
  are passed through the `headers` option, `reconnect` is on, an interrupt
  callback makes close/seek prompt.
- Read-ahead: the demuxer stops 12 s ahead or at 96 MiB of queued packets.
  Decoded video ring: 5 frames; decoded audio ring: 0.6 s.
- Video decode: VA-API hardware decoding first (`av_hwdevice_ctx_create` +
  `av_hwframe_transfer_data`), threaded software decode as fallback. The frame
  is converted to **NV12 8-bit** (swscale for 10-bit and other pixel formats).
- Audio: decoded and resampled to 48 kHz stereo float (swresample), played by an
  SDL audio callback that is the **master clock**. Video frames are presented
  when the clock reaches their pts; late frames are dropped and counted. With
  `SDL_AUDIODRIVER=dummy` the callback still runs, so timing is the same.
- Seek: the "serial" trick (ffplay): `seek()` bumps a serial, every queued
  packet/frame/sample of the old serial is discarded. Seeking lands on the
  keyframe before the target and decodes forward, dropping until the target, so
  the landing time is the exact target (within one frame).
- HLS: FFmpeg 6.1 cannot seek into unread fMP4 segments, so a seek in an HLS
  stream reopens the playlist at the target segment. The sound is held until the
  picture is ready.
- `playbackRate` 0.25-4 is done by resampling; the pitch shifts with it.
- Events are queued by the engine and drained with `poll()` in DOM order:
  loadstart, durationchange, loadedmetadata, resize, loadeddata, canplay,
  canplaythrough, play, playing, pause, waiting, seeking, seeked, timeupdate
  (~4 Hz), progress, ended, error, volumechange, ratechange.

### Shim (`src/media/shim.js`)

- Patches the prototype properties and methods (`src`, `currentTime`, `paused`,
  `readyState`, `buffered`, `play()` promise, `canPlayType`, `textTracks`,
  `requestVideoFrameCallback`, `getVideoPlaybackQuality`...) and dispatches
  the events in spec order. Native events of Chromium's own (failing) load are
  swallowed.
- `canPlayType` answers from the engine (FFmpeg decoders), including
  `application/vnd.apple.mpegurl`, so `video.src = playlist.m3u8` works.
- **MSE is told no for proprietary codecs** (`MediaSource.isTypeSupported`), and
  `Hls.isSupported()` of hls.js is forced to false, so players such as hls.js
  fall back to native HLS (`video.src = m3u8`), which the engine plays with
  seeking and audio-track switching (by reloading).
- Elements with a `blob:`/MediaStream source are left to Chromium.
- Surface: a `<canvas data-luxium-surface>` inserted after the video, positioned
  each rAF to the video's box (object-fit aware); the `<video>` itself is made
  transparent so it keeps layout and receives input. The canvas gets a WebGL2
  context; a fragment shader converts NV12 to RGB (BT.601/709/2020, limited or
  full range).
- Text tracks: `<track>` WebVTT files are fetched and parsed by the shim;
  `activeCues`, `cuechange`, `VTTCue.getCueAsHTML()` work, and `mode = "showing"`
  draws cues in the shim's own overlay. Chromium never advances cues of an
  element it does not play, hence the re-implementation.
- Controls (when `controls` is set): shadow-DOM bar with play/pause, seek bar
  with buffered ranges, time, mute/volume, fullscreen; keyboard (space/k,
  arrows +-5 s, m, f) when the video has focus.
- Fullscreen: `video.requestFullscreen()` fullscreens a wrapper the shim owns;
  pages that fullscreen their own container (homeflix) work as with Chromium.
- Chromium still fetches `src` itself once (a few tens of KiB, to sniff it) and
  gives up because it has no decoder; the engine does its own fetching.

## Wire format (`window.__luxiumMedia`)

All calls are synchronous. `id` is an integer from `create()`.

| call | returns |
| --- | --- |
| `canPlay(mime)` | `""`, `"maybe"`, `"probably"` |
| `create()` | id |
| `open(id, url, headers, allowFile)` | `headers` is a `Name: v\r\n` block |
| `close(id)`, `play(id)`, `pause(id)` | |
| `seek(id, t)`, `setVolume(id, v)`, `setMuted(id, b)`, `setRate(id, r)`, `setLoop(id, b)` | |
| `time(id)` | current time, seconds (double) |
| `state(id)` | JSON string, see below |
| `ranges(id, seekable)` | `[[start,end],...]` (buffered when `seekable` is 0) |
| `poll()` | `""` or JSON `[[id,"type","detail"],...]` for all players of the frame |
| `frame(id, afterSeq, buffer)` | see below |

`state`: `{"p":paused,"e":ended,"s":seeking,"r":readyState,"n":networkState,"c":errorCode,"w":w,"h":h,"v":hasVideo,"d":duration,"m":errorMessage}`;
`d` is absent while unknown and `null` for live streams.

`frame(id, afterSeq, buffer)` writes the frame that is due, if newer than
`afterSeq`, into the caller's `ArrayBuffer`:
`null` (nothing new), `["grow", bytes]` (buffer too small; allocate and retry) or
`[seq, w, h, displayW, pts, matrix, fullRange, bytes]`. The buffer holds
**NV12**: `w*h` bytes of Y followed by the interleaved UV plane (`ceil(w/2)*2` x
`ceil(h/2)` bytes), tightly packed. The shim uploads it with two
`texSubImage2D` calls (R8 for Y, RG8 for UV) and draws one quad.

## Every container and codec, direct (no remux, no transcode)

The engine is a plain FFmpeg player, so whatever libavformat demuxes and libavcodec decodes plays
as it is: MKV/WebM/MP4/MOV/AVI/TS/FLV/WMV/ASF/OGG..., with any codec the system FFmpeg has.

- **Decoding**: VA-API for H.264/HEVC/VP9/AV1 8-bit 4:2:0 (and 10-bit HEVC/VP9/AV1); everything else
  (Hi10P H.264, 4:2:2/4:4:4, 12-bit, ProRes, MPEG-2/4, VC-1, WMV, FLV, ...) goes straight to the threaded
  software decoder (`hw_plausible()`), and a hardware decoder that fails still falls back mid-stream.
  Output is always converted to NV12 8-bit by swscale.
- **Interlaced** pictures (decoder flag) are line-doubled from one field before conversion (no motion
  adaptation; there are no libavfilter headers in the build).
- **Audio**: any decoder; surround is downmixed to stereo float by swresample (`rematrix_maxval`), any
  sample rate is resampled to 48 kHz. The engine never outputs more than 2 channels.
- **Audio tracks inside the file**: `Player::audio_tracks()` / `select_audio(stream)`. The demuxer swaps
  the stream, the audio thread reopens its decoder and the player re-seeks to the current time (short
  `seeking`/`seeked`, the URL is not reopened). The default-flagged stream plays first.
- **Embedded text subtitles** (SRT, ASS/SSA, mov_text, WebVTT, any FFmpeg text-sub decoder): decoded in
  the demux thread into append-only cues (`subtitle_tracks()`, `sub_cues()`); ASS is reduced to WebVTT
  markup (`<i> <b> <u>`, line breaks; drawings and positioning dropped). Cues are collected as the demuxer
  reads, so after a seek past unread data the cues in the skipped part are missing until read. Bitmap
  subtitles (PGS, VobSub, DVB) are listed with `usable = false` and not rendered.
- **Capability inventory**: `media::capabilities_json()` (decoder and demuxer names as ffprobe reports
  them); `can_play()` answers for ~70 container MIME types and RFC 6381 / FFmpeg codec names.

### In the CEF shim

- `navigator.luxiumMedia` = `{engine:'luxium', version, decoders:{video,audio,subtitle}, containers,
  canDecodeVideo(name), canDecodeAudio(name), canDemux(format_name), audioTracks, embeddedSubtitles}`.
  Names are ffprobe's, so an app that probes server-side can decide without guessing.
- `HTMLMediaElement.audioTracks` (id/kind/label/language/enabled, `change`, `addtrack`); setting
  `enabled = true` on a track selects it in the engine. Embedded text subtitles appear in `textTracks`
  (mode `disabled` until the page changes it) with `VTTCue`s.
- `window.__luxiumMediaStats()` carries the engine counters (`engine`: dropped, hw, audioFrames, audioPeak...).
- Extra wire calls: `caps()`, `tracks(id)`, `selectAudio(id, stream)`, `cues(id, stream, from)`, `stats(id)`.

### In the own engine (`src/media/own_media.*`, `src/layout/media_ui.cpp`)

The control bar shows a note button with the audio stream number (only with 2+ audio streams; click =
next stream) and a `CC` button (any text subtitle stream; click cycles off/1/2/...); the active cue is
drawn centred above the bar. `examples/media/own-tracks.html` is the test page.

## Format matrix (this machine, `tools/media_matrix_fixtures.sh`, 6 s clips)

`luxium_mediatest <file> 9 -q` (engine alone) and `tools/matrixtest.py` (CEF shim, Xvfb :99, dummy audio:
canPlayType, picture, audio, tracks, subs; 0 failed checks). 0 frames dropped everywhere.

| fixture | video | audio | decode | notes |
| --- | --- | --- | --- | --- |
| MKV | HEVC 10-bit | AC-3 5.1 + AAC | VA-API | audioTracks 2, switch ok, SRT + ASS cues ok |
| MKV | H.264 | DTS | VA-API | |
| MKV | H.264 | FLAC | VA-API | |
| MKV | H.264 | TrueHD (stereo) | VA-API | |
| MKV | H.264 4:4:4 | AAC | software | |
| AVI | MPEG-4 ASP | MP3 | software | |
| MOV | ProRes 4:2:2 10-bit | PCM | software | |
| WMV | WMV2 | WMA2 | software | no VC-1 encoder here: VC-1 decoder only checked in the inventory |
| TS | MPEG-2 interlaced 720x576 | MP2 | software | deinterlace flag seen |
| FLV | FLV1 | MP3 | software | |
| FLV | H.264 | AAC | VA-API | |
| WebM | AV1 | Opus | software (libdav1d) | |
| WebM | VP9 | Vorbis | VA-API | |
| MP4 | H.264 Hi10P | E-AC-3 | software | |
| MP4 | MPEG-4 ASP | AAC | software | |

The canvas pixel check of `matrixtest.py` is skipped when the page has no WebGL2 (Xvfb here), as for
`tools/mediatest.py`, which fails that one check on the same setup.

## Limits

- 8-bit NV12 on the wire: 10-bit video is converted down in the engine (no HDR
  tone mapping; BT.2020 is only a matrix).
- One audio track plays at a time (switchable, see above); no mixing of tracks.
- `playbackRate` shifts pitch. No `captureStream`, no EME, no PiP, no WebRTC.
- No `srcObject` on shim-owned elements.
- Audio output goes to the SDL default device; there is no per-element routing
  and no Web Audio tap (not tested with `createMediaElementSource`).
- MSE for H.264/HEVC is refused on purpose; sites that cannot fall back to
  `video.src` will not play.
- Seeking in HLS reopens the playlist: ~0.2 s on LAN.

## Measured (this machine, AMD GPU with VA-API, `SDL_AUDIODRIVER=dummy`)

`luxium_mediatest <fixture> 9 --seek 3`, engine alone, localhost range server:

| fixture | first frame | dropped | seek (landing, time to `seeked`) | engine CPU |
| --- | --- | --- | --- | --- |
| H.264+AAC MP4 1080p24 | 45 ms | 0 / 227 | 2.98 s, 64 ms | 4 % of a core |
| HEVC 10-bit+AC-3 MKV 1080p24 | 41 ms | 0 / 228 | 2.98 s, 73 ms | 9 % |
| VP9+Opus WebM 720p30 | 32 ms | 0 / 284 | 2.98 s, 49 ms | 3 % |
| HEVC 4K24 MP4 | 55 ms | 0 / 223 | 2.98 s, 226 ms | 13 % |
| HLS TS 1080p | 37 ms | 0 / 225 | 2.98 s, 174 ms | 5 % |
| HLS fMP4 1080p | 37 ms | 0 / 225 | 2.98 s, 178 ms | 4 % |

Frame transport in a CEF page (`frame()` memcpy into the ArrayBuffer + texture
upload, per presented frame, from the shim's own counters):

| size | NV12 bytes | copy | upload | draw |
| --- | --- | --- | --- | --- |
| 1920x1080 | 3.1 MB | 0.4 ms | 0.2 ms | 0.5 ms |
| 3840x2160 | 12.4 MB | 1.4 ms | 0.8 ms | 0.5 ms |

Whole browser process tree while playing in a page on Xvfb (software GL, so an
upper bound; the real desktop GPU path costs less): 1080p H.264 62 %,
1080p HEVC10 67 %, 4K HEVC 76 % of one core, 24.3 fps, 0 dropped.

## Testing

- `build/luxium_mediatest URL [seconds] [--seek t,...] [--rate r] [--dump f.pgm] [-q]`
- `tools/media_fixtures.sh` builds the fixtures; `tools/mediatest.py` drives
  `examples/media/*.html` over CDP (events, time advance, pause, seek, canvas
  pixels, ended, cues, hls.js fallback). Start-up flakes (about 1 in 10: no page
  target or dropped CDP socket) are retried.
- `tools/homeflixtest.py` runs a live Homeflix (`HOMEFLIX_URL`): catalog,
  series images, direct and HEVC-remux playback, audio select, subtitle overlay,
  fullscreen with `f`, progress heartbeat.
- Run them on Xvfb (`DISPLAY=:99`), never on the real desktop, and with an own
  `LUXIUM_PROFILE` (both scripts do).
