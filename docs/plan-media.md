# Plan: Luxium's native media engine + modern-web parity (homeflix as the bar)

Goal: every page that works in desktop Chrome works in Luxium, with homeflix
(`/home/burnt/Documentos/Github/homeflix/lux-app/public`) as the acceptance
test. The gap that matters is media: stock CEF ships **without proprietary
codecs** (no H.264, HEVC, AAC, AC-3), so `<video>` in homeflix cannot play
through Chromium. Luxium gets its own media engine (our code: demux/decode
threads, clocks, A/V sync, frame delivery, element emulation, controls) on
top of FFmpeg's codec libraries, used by both engines (CEF pages and our own
HTML/CSS/LuxScript engine).

## 0. Ground rules

- Write code like the existing tree (C++23, comments in the same tone, no
  gratuitous abstraction). Comments/docs in English like the recent commits.
- Never add `Co-Authored-By` or any Claude attribution to commits.
- Commit in coherent steps on `master` and push (`origin` =
  github.com/Eldur4023/Luxium). Each commit must build.
- Do not take desktop screenshots. Verify with: CDP (`--remote-debugging-port`
  on CEF, evaluate JS), Luxium's own frame dumps (`copy_frame` → PPM, `--shot`),
  logs, headless tools. Dumping Luxium's *own* framebuffer to a file is fine.
- Optional deps follow the CEF pattern: CMake toggle + stub when absent, the
  build never breaks for lack of them.

## 1. Dependencies (no sudo available)

- **CEF**: `third_party/cef` is gitignored and currently missing. Download the
  latest stable *minimal* linux64 dist from
  `https://cef-builds.spotifycdn.com/index.json` (ROADMAP mentions 154) and
  extract into `third_party/cef`. Add `tools/fetch_cef.sh` that does this so it
  is reproducible. Check the existing `cef_backend.cpp` still compiles against
  it (API drift between CEF versions: fix as needed).
- **FFmpeg headers**: runtime libs exist (`libavcodec.so.60`, avformat 60,
  avutil 58, swscale 7, swresample 4, libva 2 — FFmpeg 6.1). Dev packages are
  not installed and there is no sudo: `apt-get download libavcodec-dev
  libavformat-dev libavutil-dev libswscale-dev libswresample-dev` (+ `libva-dev`
  if VA-API needs headers) and `dpkg -x` them into `third_party/ffmpeg`
  (gitignored). Add `tools/fetch_ffmpeg_dev.sh`. CMake: `LUXIUM_MEDIA` ON when
  those headers exist; link the system `.so.60` etc.
- GPU is AMD (VA-API). Hardware decode via `av_hwdevice_ctx_create(VAAPI)` +
  `av_hwframe_transfer_data`, falling back to threaded software decode.

## 2. `src/media/` — the engine (browser process)

One `Player` per media element. Public surface (`src/media/player.hpp`):
`open(url, headers)`, `play()`, `pause()`, `seek(seconds)`, `set_volume`,
`set_muted`, `set_rate` (0.25–4; may shift pitch — document it), `close()`;
state getters (duration, current time, paused, ended, seeking, ready state,
network state, buffered ranges, video size, error code/message); an event
queue drained by the owner (loadstart, durationchange, loadedmetadata,
loadeddata, canplay, canplaythrough, play, playing, pause, waiting, seeking,
seeked, timeupdate ~4 Hz, progress, ended, error, volumechange, ratechange,
resize).

- **Demux thread**: libavformat opens any URL it supports — progressive MP4,
  MKV, WebM, MPEG-TS, and HLS `.m3u8` (FFmpeg's hls demuxer) over http(s).
  Pass request headers (`Cookie`, `Referer`, `User-Agent`, `Authorization` when
  the page has them) via the `headers` AVOption; `reconnect` options on. Packet
  queues per stream with byte/duration caps; interrupt callback for prompt
  close/seek. Buffered ranges from the queued packets' pts.
- **Video decode thread**: hwaccel or sw; output always **NV12 8-bit** (convert
  P010 / yuv420p / anything else with swscale). Keep colorspace (BT.601/709/2020)
  and range (limited/full) with each frame. Small ring of decoded frames.
- **Audio**: decode + swresample to F32 stereo 48 kHz; output via
  `SDL_OpenAudioDevice` (SDL is already a dependency). Audio clock is the
  master; video presents the frame whose pts ≤ audio clock and drops late ones.
  No audio stream → wall clock. Volume/mute applied in the callback.
- **Seek**: flush queues + decoders, `avformat_seek_file` backward to keyframe,
  decode-and-discard up to the target, then `seeked`.
- **Errors** map to MediaError codes (1 aborted, 2 network, 3 decode,
  4 src not supported).
- Expose `media::can_play(mime, codecs)` from the decoders FFmpeg actually has
  (h264, hevc, av1, vp8/9, aac, mp3, opus, vorbis, ac3, eac3, flac; HLS).
- Unit-ish self-test: `luxium_mediatest file_or_url` (headless, no window, SDL
  dummy audio) that plays N seconds and prints events + frames presented +
  dropped. Generate fixtures with the system `ffmpeg` (testsrc/sine): H.264+AAC
  MP4, HEVC 10-bit+AC-3 MKV, an HLS fMP4 playlist, an HLS TS playlist, VP9 WebM.

## 3. CEF pages: the media bridge

Why not composite natively over the OSR frame: page content drawn *above* the
video (homeflix's subtitle overlay, menus, our own controls) would be covered.
So frames are drawn *inside the page*.

- **Renderer side** (`CefRenderProcessHandler`, in the subprocess): on
  `OnContextCreated` of every frame inject `src/media/shim.js` (embedded in the
  binary as a generated `.inc`) plus a native V8 function
  `__luxiumMedia.post(json)` → `CefProcessMessage` to the browser process.
  Browser → renderer messages are dispatched to the shim's callback. The
  decoder capability list goes to the renderer at startup (extra_info or a
  command-line switch) so `canPlayType` is synchronous.
- **Browser side**: a player registry keyed by (browser, frame, element id);
  players die on `OnContextReleased`, navigation, browser close. Cookies for
  the media URL come from `CefCookieManager::VisitUrlCookies`.
- **Frame transport**: custom scheme `luxium-media://` (registered as standard,
  secure, CORS + fetch enabled) served by a `CefResourceHandler` returning the
  latest NV12 frame of a player as raw bytes with a small header (w, h, strides,
  pts, colorspace, range, seq). The shim pulls it on `requestAnimationFrame`
  only when `seq` changed (long-poll / `If-None-Match`, whatever is cheapest),
  and uploads Y (R8) + UV (RG8) to WebGL2 textures; a shader does YUV→RGB with
  the right matrix/range. Measure the per-frame cost for 1080p and 4K (log it);
  if 4K at 24 fps does not keep up, document the limit.
- **shim.js** patches `HTMLMediaElement.prototype` (not the page's code):
  `src`/`currentSrc` (+ `<source>` children and `type` attributes), `load`,
  `play` (returns a Promise; autoplay is allowed), `pause`, `currentTime`,
  `duration`, `paused`, `ended`, `seeking`, `readyState`, `networkState`,
  `buffered`/`seekable`/`played` (TimeRanges-like), `volume`, `muted`,
  `defaultMuted`, `playbackRate`, `error`, `videoWidth`/`videoHeight`,
  `canPlayType`, `autoplay`, `preload`, `poster`, `loop`. Events are real DOM
  events dispatched on the element, in spec order.
  - `MediaSource.isTypeSupported` returns false for proprietary codecs so MSE
    players (hls.js) fall back to native HLS (`canPlayType("application/vnd.apple.mpegurl")`
    → `"maybe"`). Open-codec MSE stays Chromium's.
  - **Text tracks**: Chromium will not advance cues of a fake media element, so
    the shim implements `textTracks` (TextTrackList-like: length, index access,
    iteration, `change`/`addtrack` events), parses `<track src>` WebVTT itself
    (fetch), builds real `VTTCue` objects (so `getCueAsHTML()` works), keeps
    `mode` (disabled/hidden/showing), `activeCues`, `cuechange`, and renders
    `showing` tracks itself. Handle tracks added/removed dynamically
    (MutationObserver) and `addTextTrack`.
  - **Surface**: a `<canvas>` inserted as the video's next sibling,
    `position:absolute; pointer-events:none`, geometry synced each rAF to the
    video's box (offsetParent-relative, respect `object-fit`/letterboxing,
    `display:none`, `visibility`); the `<video>` itself is made transparent
    (keeps layout and receives clicks/dblclick). Poster drawn until the first
    frame.
  - **Controls**: when `controls` is set (and respecting `controlslist`), the
    shim draws its own bar in a shadow-DOM host over the video: play/pause,
    seek bar with buffered ranges and hover time, current/total time,
    mute + volume slider, fullscreen; autohide after ~2.5 s without pointer
    movement; keyboard (space/k, arrows ±5 s, m, f) when the video is focused.
  - **Fullscreen**: `video.requestFullscreen()` fullscreens a wrapper the shim
    owns (so the canvas and controls come along); `document.fullscreenElement`
    still reports what the page expects as far as possible.
    `CefDisplayHandler::OnFullscreenModeChange` → the shell goes
    `SDL_WINDOW_FULLSCREEN_DESKTOP` and hides its chrome; Esc exits.
  - Element removed from the DOM → pause; GC'd/unloaded → player released.
  - `<audio>` uses the same path without a surface.

## 4. Modern-web parity sweep (CEF)

Check each; fix what fails:
- Persistent profile: `root_cache_path`/`cache_path` under
  `~/.local/share/luxium/profile` so cookies + localStorage survive restarts
  (homeflix keeps its user in localStorage).
- `<input type=file>` (homeflix uploads covers): if OSR has no dialog, add a
  `CefDialogHandler` (xdg-desktop-portal via `gdbus`, or `zenity`/`kdialog`).
- JS dialogs (alert/confirm/prompt) work in OSR (`CefJSDialogHandler` if not).
- `window.open`/`target=_blank` → open in the same window (or new tab if tabs
  exist); downloads (`CefDownloadHandler` → `~/Descargas` / XDG download dir).
- Cursor shape mapping (`cef::cursor` → SDL cursors), IME/composition, wheel
  smoothness, HiDPI (device scale factor), clipboard (copy/paste in inputs and
  `navigator.clipboard.writeText`), WebSocket, `fetch`, `EventSource`.
- Key events reach the page (homeflix uses `f`, arrows, space).

## 5. Own engine (non-JS pages)

`<video>`/`<audio>` in our HTML/CSS engine use the same `media::Player`:
replaced box with intrinsic size (300×150 default, then video size), frames
converted to BGRA (swscale) and painted in the display list, simple controls
(click toggles play, a progress bar), `autoplay`, `controls`, `loop`, `muted`,
`poster`. LuxScript: `element.play()/pause()/seek(t)/current_time()/duration()`
in `browser_api.inc` — that file is owned by Lux (see the vendoring table in
docs); add the declarations in Lux's `luxium` branch first, then
`tools/sync_from_lux.sh`. If that cross-repo step is too large, ship the
rendering/controls part and leave the LuxScript API documented as TODO.

## 6. Acceptance

1. `luxium_mediatest` plays every fixture: events in order, frames presented,
   dropped < 2 % for 1080p, seek lands within one GOP, HLS works.
2. Fixture pages in `examples/media/` (one per fixture, plus one with hls.js
   from homeflix's `hls.min.js`, plus one with `<track>` VTT) verified through
   CDP: `currentTime` advances, `paused` toggles, events fire, `textTracks[0].activeCues`
   changes over time, canvas pixels are not black (`readPixels`/`toDataURL`).
3. **Homeflix**: serve the frontend from the live server
   `http://192.168.1.20:5001` (or 9700). Through CDP: catalog renders, a series
   page loads its images, an episode plays (HEVC remux path and direct path),
   audio select switches, subtitles show, fullscreen toggles, progress is
   reported. If a login/user selection is needed, use the profile picker in the
   UI; if credentials are required, stop and report instead of guessing.
4. Update `README.md`, `docs/ROADMAP.md` (new milestone: M7 media + parity) and
   write `docs/MEDIA.md` (architecture, wire format, limits, measured numbers).

## 7. Out of scope

EME/DRM, WebRTC, MediaRecorder, Picture-in-Picture, multiple audio tracks
inside one file (homeflix switches tracks server-side), Windows.
