# Luxium

A web browser for HTML + CSS + **LuxScript**. The own engine runs no JavaScript;
pages that need it are handed to Chromium (CEF), which is loaded lazily as a
plugin, and its video goes through Luxium's native FFmpeg media engine.

Luxium is the client-side sibling of [Lux](../Lux): where Lux serves pages
whose logic is written in Lux Script, Luxium renders pages whose logic is also
written in Lux Script. Same language, same grammar, same bytecode VM — the
reserved objects change (`request`, `app`, `routes` → `document`, `page`),
because the context is a browser, not a server.

## Status: M0–M7 — usable browser, app host and SPA runtime

The browser opens a window, renders a page, and **runs its LuxScript**:

- HTML: h1–h3, p, ul/li, buttons, hr, br, b/i/code, blockquote, inline
  `style="color: #rrggbb"` — block layout, word wrap, scroll, scrollbar.
- LuxScript: every `<script>` block compiles at page load with Lux's own
  pipeline (lexer → parser → checker → IR → bytecode) and runs on Lux's own
  VM. `while` loops are bounded by the VM's step limit, so a page cannot hang
  the browser.
- Browser surface: `log.info/warn/error` (console pane), `document.get /
  query_all / title / root`, `element.text / set_text / click / attr /
  set_attr / tag / id / child_count`, `page.get / page.set` (the page-scoped
  store — the browser's `state.*`). Nothing else exists to call: the sandbox
  is the API surface.
- `element.click(fn)` handlers run on real mouse clicks and mutate the DOM:
  `document.create`, `element.append/remove`, attributes, `text-align`,
  `background-color`, `display:none`, underlined links — a mutation
  relayouts and repaints.
- `page.after(ms, fn)` timers drive live pages (the todo demo updates a
  counter every 400ms).
- Entities decode (`&middot;` & friends, `&#123;`).
- `luxium --check dir/` lints every page (exit code for scripts).
- `.luxp`, Luxium's own app format (DOM + parsed stylesheets + verified bytecode), produced by Lux: `lux pack page.html app.luxp`, or any Lux `render()` route when Luxium asks for it (`Accept: application/x-luxp`). `luxium app.luxp` / `luxium http://…` run it with no HTML/CSS parsing or compiling; `luxium --inspect app.luxp` lists what it can do. See `docs/LUXP.md`.
- **Navigation**: `<a href>` between local pages and real http(s):// URLs
  (libcurl on a worker thread, error pages on failure), history
  (Alt+Left/Right, mouse buttons), reload (Ctrl+R), `#anchors`,
  view-source (Ctrl+U), status bar with the hovered link.

Since M6/M7 (see [docs/ROADMAP.md](docs/ROADMAP.md)): a native FFmpeg media engine
(any container/codec, VA-API, HLS, audio/subtitle tracks, `<video>`/`<audio>` in the own
engine), the app host (`luxium --host`: one chrome-less window per client app, own-engine
windows, lazy Chromium, 30/42/66 MB idle for 1/2/4 apps), a LuxScript SPA runtime (async
handlers, http/ws/storage/history/events/forms) and engine-drawn forms. CSS animations,
CSS animations/`@keyframes`, SVG, colour emoji, filters, masks, container queries and Color 4/5 are in; Windows and native Wayland are open.

The showcase is `examples/counter.html`: two buttons wired in Lux Script,
state in the page store, the counter updates on screen.

## Build and run

```sh
./compile.sh
./build/luxium examples/counter.html        # the window
./build/luxium examples/hello2.html         # the long page
```

Deps: SDL2, FreeType, HarfBuzz (plus a C++23 compiler).

Headless tools, for working without a display:

```sh
./build/luxium page.html --headless               # console output only
./build/luxium page.html --click mas --shot out.ppm   # one frame to a PPM
./build/luxium_headless page.html id [id...]      # load + click ids
```

`--shot` runs on SDL's dummy driver: that is how the rendering is tested
without stealing a screen, and how this README's claims were verified.

## Layout

```
docs/               the plan: architecture, roadmap, LuxScript-in-browser spec
src/dom/            DOM tree (Node, Document)
src/html/           HTML tokenizer + tree builder
src/lux_script/     Lux's engine, vendored (lexer/parser/IR/emitter/bytecode/VM)
                    with browser natives — see docs/ARCHITECTURE.md §4
src/script/         compile driver + per-page Engine (the Host the VM talks to)
src/page/           the page model: DOM + scripts + console
src/render/         text (FreeType+HarfBuzz, glyph cache)
src/layout/ src/paint/ src/style/   layout, painting and the CSS cascade
src/shell/          SDL2 shell: framebuffer, event loop, console pane
src/media/          FFmpeg media engine, own-engine <video>/<audio>, CEF shim.js
src/host/           app host (luxium --host): windows, menus, tray
src/cef/            Chromium (CEF) backend, lazy plugin libluxium_cef.so, media bridge
src/tools/          headless harness
examples/           demo pages
```

## Documents

| Document | What it covers |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, components, engine strategy, sandbox model |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M0–M7 with scope and acceptance criteria |
| [docs/RENDERING.md](docs/RENDERING.md) | The rendering engine, the Chromium oracle and the test harness |
| [docs/LUXSCRIPT.md](docs/LUXSCRIPT.md) | How Lux Script embeds in a page and what changes vs. the server |

## Tests against Chromium

`css-tests/` holds 136 cases (135 pass; see docs/RENDERING.md for the known gaps) (one CSS feature per page, plus responsive,
real-page and LuxScript-mutation cases).  Each is rendered by Luxium's own
engine and by Chromium (CEF) and compared on box rects (±1px), computed styles,
pixels and colour:

```sh
python3 tools/cssrun.py                 # all of them
python3 tools/cssrun.py -v flex         # per-element detail
python3 tools/cssrun.py --check-baseline
```

See [docs/RENDERING.md](docs/RENDERING.md).
