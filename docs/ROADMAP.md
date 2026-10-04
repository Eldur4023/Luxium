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

## M1 — Text renderer ("something on screen")

HTML in, pixels out. Layout of block and inline boxes, text only.

Scope:
- Shell window (toolkit per ARCHITECTURE §8) + software framebuffer.
- Box tree: block layout (stacking), inline layout (words, wrapping at the
  viewport edge), fixed DPI.
- Painted: text with one font/size, `body` background color, nothing else.
- Scrollbar for vertical overflow (even a crude one).
- A test page corpus under `examples/` that must render readably.

Non-goals: CSS (colors beyond body background come from defaults), images,
selection, input.

Acceptance:
- [ ] A `examples/*.html` page with headings, paragraphs and lists renders
      legibly and scrolls.
- [ ] Malformed input renders *something* and logs every dropped node.
- [ ] Repaint on resize; no tearing artifacts visible on interactive scroll.

## M2 — LuxScript executes

The reason this browser exists. Vendor the Lux frontend + VM, wire the first
browser natives.

Scope:
- Vendor `../Lux` frontend (lexer, parser, ast, ir, emitter, bytecode, value,
  vm, diagnostic, type) into `src/luxscript/` as `libluxscript`; delete the
  M0 lexer skeleton.
- `luxium::script::Engine`: compile-at-parse, one VM per page, run top-level
  after the DOM is complete.
- Natives: `log.*` (to a console pane in the shell) and read-only `document`
  (title, element counts, `get(id)` → opaque handle, text reading).
- Instruction budget per run; budget exhaustion = script killed + diagnostic.
- Script compile errors and runtime errors surface in the console pane with
  line/col, never in a dialog.

Non-goals: DOM mutation, events, timers, fetch.

Acceptance:
- [ ] A page with `<script>` doing arithmetic, strings, lists, dicts, loops
      and a user `fn` runs on load; its `log` output appears in order.
- [ ] A compile error (bad type, unknown method) prints one precise
      diagnostic; the page still renders its HTML.
- [ ] An infinite loop is killed by the budget; the window stays responsive.
- [ ] `document.title` and `document.get("x").text` work from a script.

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
