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

## M3 — CSS subset

Scope:
- CSS tokenizer/parser (subset: rules, declarations; no at-rules except
  maybe `@media` later).
- Selectors: tag, `.class`, `#id`, descendant combinator.
- Cascade: specificity, source order, inheritance for inherited properties.
- Properties: `display` (block/inline/none), `color`, `background-color`,
  `font-size`, `font-weight`, `margin`, `padding`, `width`, `height`,
  `text-align`, `border` (solid, single color).
- Inline `<style>` blocks; `<link rel=stylesheet>` for local files.

Non-goals: flexbox/grid (M6 candidates), floats, positioning, transitions,
vendor prefixes, `!important` (decide then).

Acceptance:
- [ ] The M1 corpus restyled with CSS looks like the same document properly
      typeset: margins collapse, headings sized, lists indented.
- [ ] Selector specificity resolved per spec order for the supported subset.

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

## M5 — Page model + networking

Scope:
- Links (`<a href>`), history (back/forward), reload; `file://` first.
- Minimal http/1.1 GET on worker threads (or libcurl — decide by code size),
  delivered as events to the UI thread; status page for network errors.
- View-source view (pretty-printed DOM dump — the M0 CLI output in a tab).
- Console pane graduates to a real devtools tab: DOM tree, script errors,
  network log.

Non-goals: cookies, cache, TLS beyond what the HTTP library gives for free,
service workers (forever).

Acceptance:
- [ ] Clicking between local pages keeps back/forward working.
- [ ] An http page renders; a dead server shows a readable error page.
- [ ] View-source matches the CLI dump for the same file.

## M6 — Candidates (unordered, pick when M5 lands)

Forms (input/text/checkbox, LuxScript-validated submission to Lux servers —
the round trip with `../Lux` as the origin server is the showcase), flexbox,
images (PNG/JPEG via stb or similar), `<table>` layout completion,
`@media`/viewport sizes, text selection + clipboard, the `--native` question
(should Luxium *cache compiled bytecode* per page like a build artifact?),
GPU painting if the rasterizer profiles hot.
