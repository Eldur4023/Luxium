# Luxium — Roadmap

Each milestone lists scope, explicit non-goals, and acceptance criteria. A
milestone is done when every acceptance line passes, not before. Dates are
deliberately absent: scope per session beats deadlines.

---

## M0 — Skeleton (done)

Two pipelines proven end to end, zero UI.

- [x] `luxium file.html` reads a file, tokenizes HTML, builds a DOM.
- [x] DOM tree dumps to the terminal (elements, attributes, text, comments).
- [x] Every `<script>` block is lexed with the Lux Script lexer skeleton;
      tokens and lexing diagnostics are printed.
- [x] Malformed HTML never crashes: recovery + diagnostics on stderr.
- [x] Builds clean (`-Wall -Wextra`) with CMake, C++23, gcc 13.

Non-goals: rendering, CSS, script *execution*, networking, windows.

## M1 — Text renderer ("something on screen") — DONE

HTML in, pixels out. Block and inline boxes, real text, real window.

Shipped:
- [x] SDL2 shell window + ARGB software framebuffer; repaint on resize.
- [x] Block layout (per-tag sizes/margins), inline words, wrap at the
      viewport edge; DejaVu Sans regular+bold via FreeType, shaped with
      HarfBuzz, glyphs rasterized once per (glyph, bold) and cached.
- [x] Vertical scroll (wheel, PgUp/PgDn, Home/End) + scrollbar.
- [x] Buttons painted as boxes with their text inside; hr rules.
- [x] The `examples/` corpus renders legibly — verified pixel-by-pixel via
      the `--shot` PPM path on SDL's dummy driver.

Deferred to later milestones (was in scope here, moved on purpose): text
selection, per-word damage tracking, DPI scaling beyond the default.

## M2 — LuxScript executes — DONE

The reason this browser exists. Lux's frontend + VM vendored, browser natives
wired, and (ahead of the original scope) the DOM is already mutable and
clickable, because a browser whose scripts cannot touch the page is a viewer.

Shipped:
- [x] `../Lux`'s engine vendored verbatim into `src/lux_script/` (lexer,
      parser, checker, IR, emitter, bytecode, value, VM); `template.cpp`
      stubbed so `render()` is a precise compile-time "does not exist".
- [x] `script::Engine`: all `<script>` blocks compile as ONE shared-scope
      program (declarations hoisted to top level, HTML margin stripped,
      diagnostics remapped to document lines); top-level runs after the DOM
      is complete; one VM per run, nothing shared between pages.
- [x] Natives: `log.*` → console pane; read + write `document`;
      `element.click(fn)` handlers; `page.get/page.set` page-scoped store
      (the browser's `state.*` — a script's top level is a fn body, so its
      locals cannot be handler state).
- [x] Infinite loops die on the VM's own step limit ("infinite loop?") —
      the budget already existed in Lux's VM; no patch was needed.
- [x] Compile and runtime errors surface in the console pane with the
      page's line numbers; the HTML still renders.

Acceptance (all verified):
- [x] `examples/engine-test.html`: arithmetic, strings, lists, loops, user
      `fn`s, two `<script>` blocks sharing scope — log output in order.
- [x] A compile error prints one precise diagnostic with the page's line;
      the page still renders.
- [x] An infinite `while` is killed by the step limit.
- [x] `document.title`, `document.get("x").text` from a script.
- [x] `examples/counter.html`: clicks mutate the DOM, the counter updates
      on screen (verified via `--shot` after simulated clicks).

## M3 — CSS subset — DONE (the subset that shipped)

The browser's own default look is ITSELF a stylesheet: `kUAStylesheet` in
layout.cpp is parsed with the same engine as page CSS, so a page rule wins
purely by specificity — zero special cases.

Shipped:
- [x] `src/css/`: parser (rules, comma selector lists, descendant
      combinator, at-rules skipped, comments stripped, malformed rules
      degrade) + cascade: specificity (id/class/tag), source order,
      per-property inheritance (color/size/line-height/family/align/weight
      flow down; boxes do not).
- [x] Selectors: tag, `.class`, `#id`, descendant chains, comma lists.
- [x] Properties: `color`, `background-color` (block boxes AND inline
      pills), `font-size` (px/em/rem/%), `font-weight`, `font-family`
      (mono detection), `line-height`, `text-align` (left/center/right),
      `display: none` (subtree gone), `margin*`, `padding*` (block box
      model: background + padding).
- [x] `<style>` blocks and `<link rel=stylesheet>` (local files).
- [x] `style=""` inline attributes through the same declaration parser.
- [x] Button box color overridable with `background-color`.

Found and fixed while building it: the compound-selector parser dropped
`.`/`#` markers (`section.oscuro` parsed as tag `section`, so one rule
styled every section and `display:none` never hid anything) — the probe
now prints parsed chains, which is how it was caught.

Still open in CSS-land (M6 candidates): flexbox/grid, floats, specific
borders, `!important`, `@media`.

## M4 — DOM mutation + events (the page wakes up)

Scope:
- Natives: `element.set_text`, `create/remove element`, attribute edit;
  invalidation flows node → style → layout → paint.
- `element.on("click", fn)` on a first set of elements; hit-testing from the
  framebuffer mouse position to the box tree.
- `page.after(ms, fn)` timers on the UI thread's event queue.
- Instruction budget applies per handler invocation.

Non-goals: fetch, forms submission, focus/tab order beyond the trivial.

Acceptance:
- [ ] A counter page: click a button, a `<span>` updates, layout reflows.
- [ ] A todo-list page (add/remove items) works entirely in LuxScript.
- [ ] A leaking handler (allocating every timer tick) is detectable in
      devtools memory printout — kill switch if not.

## M5 — Page model + networking — DONE (core)

Shipped:
- [x] `<a href>` clicks resolve against the current page: relative file
      paths (with normalization, `..` included), `file://`, absolute paths,
      `http(s)://`, `#anchors` (scroll to the id), and `javascript:` URLs
      are refused with a console message.
- [x] History: back/forward (Alt+Left/Right, mouse X1/X2), reload
      (Ctrl+R/F5).
- [x] HTTP(S) via libcurl on a worker thread (one fetch at a time; the UI
      never blocks; redirects, timeouts, 32MB cap); the page lands through
      the normal load pipeline -- its LuxScript compiles and runs.
- [x] Error pages for dead servers, missing files, HTTP >= 400.
- [x] `luxium http://...` starts the browser on a URL.
- [x] Status bar: hovered link / "cargando ..." / page name.
- [x] View-source on Ctrl+U (the real source, entity-escaped, in a pre).

Still open (M6): devtools tab with DOM tree + network log, page cache,
cookies, forms.

Acceptance (all verified via simulated clicks + framebuffer inspection):
- [x] `examples/index.html` links into the local pages; navigation renders
      the target (shot after simulated click).
- [x] `#contacto` anchor scrolls the document.
- [x] `luxium http://127.0.0.1:8077/counter.html` renders a page served by
      a local http server; a dead port renders the error page.

## M6 — Dual engine: Chromium off-screen (OSR) — DONE

Luxium judges every page: if it asks for JavaScript (explicit
`type="...javascript"`, an external `<script src>` that is not `.lux`, or a
plain `<script>` that fails to compile as LuxScript) the URL is handed to
Chromium through CEF; everything else renders with the own engine. The
chrome — URL bar, status line, console pane — is always ours.

Shipped:
- [x] CEF 154 vendored in `third_party/cef` (gitignored); CMake toggle
      `LUXIUM_CEF` with a no-op stub backend when the dist is absent.
- [x] Windowless (OSR) rendering: Chromium creates NO X11 window. A native
      child was proven impossible — X11 children must share the parent's
      visual when depths match, SDL picks 0x3ba and Chromium hardcodes the
      default 0x21, so every CreateWindow died in BadMatch. Instead
      `CefRenderHandler::OnPaint` delivers BGRA frames and we blit them
      into our framebuffer, page area only.
- [x] Input forwarded from our SDL loop via CefBrowserHost: mouse move /
      click / wheel inside the page area, raw keys + CHAR events (Windows
      VK table + X11 native keycodes), focus, engine-aware Esc (goes to
      the page, not the app) and Chrome shortcuts (Ctrl+L/R/U,
      Alt+arrows).
- [x] Engine-aware chrome: the address bar shows `cef::url()`, the window
      title follows OnTitleChange, Chromium's console messages land in our
      console pane, back/forward/reload route to the right engine.
- [x] Bug found by instrumentation: `set_view` computed `a && b` over two
      `exchange`s — the first comparison short-circuited the second, the
      height stayed at its initial 1 and Chromium painted a 1px strip
      (the "blank screen" report).
- [x] The console pane is hidden by default: F12 toggles it (works even
      with the address bar focused), a thin 20px status strip stays at the
      bottom edge always, and the page gets the rest. Toggling re-runs
      ensure_surface, so Chromium re-renders at the new page size
      (OnPaint 1180x762 <-> 1180x549 verified).
- [x] The URL bar is a real text field: caret placed by clicking (word on
      double-click, all on triple), arrows/Home/End with shift-selection,
      Ctrl+A/C/X/V through the SDL clipboard (paste strips CR/LF and
      stray edge spaces), horizontal scroll so long addresses stay
      reachable, selection painted under the text, blinking caret.
      Verified deterministically with on-demand frame dumps
      (`LUXIUM_DUMP_TRIGGER`): click→caret pixel, mid-text insert,
      selection paint, an exact 32-char clipboard roundtrip, Enter
      navigation.

Acceptance (verified visually through framebuffer dumps + xdotool input):
- [x] `examples/js-test.html` (`type="text/javascript"`) renders through
      Chromium: the crimson h1 written by the page's own JS appears inside
      our chrome.
- [x] `http://google.es` loads, redirects and renders (logo, search box,
      autocomplete); xdotool click + typing lands in the box, Enter
      searches — and a hand-driven Google search succeeded.
- [x] Chromium console messages appear in our console panel.
- [x] `--check examples/` still 0 errors; the `--shot` path is untouched
      (the dummy driver never hands off).

Still open (M8 candidates): cursor-shape mapping (`cef::cursor` → SDL),
wheel sign on long pages, text selection + copy/paste in the own engine.

## M7 — Native media engine + modern web + SPA runtime (mostly done)

What exists today (details: `docs/MEDIA.md`, `docs/HOST.md`, `docs/LUXSCRIPT.md` §3.1,
`docs/RENDERING.md`).

Done:
- [x] **FFmpeg media engine** (`src/media/player.cpp`): every container/codec played
      directly (no browser codec whitelist), VA-API hardware decode with software
      fallback, A/V sync on the audio clock, HLS (TS and fMP4), seeking, in-file audio
      track switching and embedded text subtitles, deinterlace. Without FFmpeg headers a
      stub keeps the build working.
- [x] **CEF bridge, lazy plugin** (`src/cef/`): Chromium lives in `libluxium_cef.so`,
      loaded on the first JavaScript page; a JS shim replaces `HTMLMediaElement` so
      Chromium pages play through the native engine. CEF parity sweep
      (`tools/paritytest.py`) green.
- [x] **Own-engine `<video>`/`<audio>`** (`src/media/own_media.*`, controls, track
      buttons, cue rendering), usable from LuxScript (`play pause seek current_time
      duration paused ...`, tested on a real file in `tools/spatest.py`).
- [x] **App host** (`luxium --host`, `src/host/`): one chrome-less SDL window per client
      app over a unix socket; own-engine windows by default, Chromium only for
      JavaScript pages and loaded lazily; menu bar with accelerators, tray, dark title
      bar, window icon. Measured idle private PSS: 30 / 42 / 66 MB for 1 / 2 / 4 apps.
- [x] **SPA runtime**: coroutine handlers/timers (`await`), `http` (curl workers),
      WebSocket client, storage, history/navigation, capture/bubble events, forms and
      multipart upload, templates, element API. `tools/spatest.py`: 45 checks.
- [x] **Engine forms + CSS**: form controls drawn by the engine, caret/selection/IME,
      scroll containers, `:has`/`:focus-within`, `min/max/clamp`, sticky, transitions;
      `css-tests/` 118 cases against Chromium.
- [x] **Homeflix-Local ported to LuxScript** and running on the own engine
      (`tools/homeflixsnap.py` goldens).

In progress (another agent, not landed): CSS animations (`@keyframes`), SVG, colour emoji.

Open:
- [ ] Windows build/host (the CEF side has a Win path; the own engine/host is Linux/X11 only).
- [ ] Native Wayland (runs through XWayland).
- [ ] HiDPI for the own engine (scale > 1); today only CEF pages get page zoom.
- [ ] IME preedit and real-HiDPI sharpness are not testable headless.

## M8 — Candidates (unordered)

Forms (input/text/checkbox, LuxScript-validated submission to Lux servers —
the round trip with `../Lux` as the origin server is the showcase), flexbox,
images (PNG/JPEG via stb or similar), `<table>` layout completion,
`@media`/viewport sizes, text selection + clipboard, the `--native` question
(should Luxium *cache compiled bytecode* per page like a build artifact?),
GPU painting if the rasterizer profiles hot.

## CEF parity sweep (plan-media section 4)

`tools/paritytest.py` (needs Xvfb + xdotool, `DISPLAY=:99`) drives a real
Luxium with real input and CDP; all checks pass: persistent profile (cookies +
localStorage, `LUXIUM_PROFILE`), file dialog / JS dialogs (zenity), downloads
(XDG dir), `window.open`/`_blank` in the same window, cursor mapping, clipboard
(Ctrl+C/X/V/A + `navigator.clipboard`), key/wheel events, fetch / EventSource /
WebSocket, element fullscreen (window fullscreen, chrome hidden, Esc exits),
non-ASCII text via the IME commit path.  HiDPI (`LUXIUM_SCALE` or Xft.dpi) is
applied as page zoom.  Not testable headless: IME preedit with a real input
method, HiDPI sharpness on a real HiDPI screen.
