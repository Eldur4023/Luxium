# Luxium

A web browser for HTML + CSS + **LuxScript** — no JavaScript.

Luxium is the client-side sibling of [Lux](../Lux): where Lux serves pages
whose logic is written in Lux Script, Luxium renders pages whose logic is also
written in Lux Script. Same language, same grammar, same bytecode VM — the
reserved objects change (`request`, `app`, `routes` → `document`, `page`),
because the context is a browser, not a server.

## Status: M0–M2 — usable demo

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
- `element.click(fn)` handlers run on real mouse clicks and mutate the DOM;
  a mutation relayouts and repaints.

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
src/render/         text (FreeType+HarfBuzz, glyph cache) and layout (boxes)
src/shell/          SDL2 shell: framebuffer, event loop, console pane
src/tools/          headless harness
examples/           demo pages
```

## Documents

| Document | What it covers |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, components, engine strategy, sandbox model |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M0–M6 with scope and acceptance criteria |
| [docs/LUXSCRIPT.md](docs/LUXSCRIPT.md) | How Lux Script embeds in a page and what changes vs. the server |
