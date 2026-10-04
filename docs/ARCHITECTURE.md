# Luxium — Architecture

> What Luxium is, what it is not, and how the pieces fit. This document is the
> reference for *why* the code is shaped the way it is. The sequence in which
> the pieces get built lives in [ROADMAP.md](ROADMAP.md); the language rules in
> [LUXSCRIPT.md](LUXSCRIPT.md).

---

## 1. What Luxium is

A browser that renders **HTML + CSS + LuxScript**. LuxScript replaces
JavaScript end to end: there is no second language, no JIT-for-someone-else's-
VM, no `eval` of untyped dynamic code. Pages carry Lux Script source; Luxium
compiles it at load time with the same pipeline Lux the server uses — lexer,
parser, IR, bytecode — and runs it on the same VM design.

What it is **not** (yet, and some things never):

- Not a JavaScript browser. There is no JS engine and none is planned.
- Not a general HTML spec implementation. The WHATWG parsing algorithm is
  simplified deliberately (see §5); CSS arrives in M3 as a useful subset.
- Not a network client yet. Pages come from local files until M5.

## 2. The layers

```
+------------------------------------------------------------------+
|  Shell: window, tabs, devtools, preferences                       |
|  (a thin native UI; toolkit decision in §8 — not built yet)       |
+------------------------------------------------------------------+
|  Page model: page = DOM + CSSOM + scripts + scroll state          |
+------------------------------------------------------------------+
|  Networking: file:// now, http(s) in M5                           |
+------------------------------------------------------------------+
|  HTML parser  ──────►  DOM                                        |
|  CSS parser   ──────►  CSSOM ──►  style engine (matched rules)    |
|                                                │                  |
|  LuxScript engine:  source → lexer → parser → IR → bytecode       |
|                     bytecode → VM  ◄──── browser natives          |
|                                                │                  |
|  Layout: DOM + styles → box tree → geometry                       |
|  Paint:  box tree → software rasterizer → framebuffer             |
+------------------------------------------------------------------+
```

Reading order for a page load (M1–M2 target):

1. Fetch bytes (file read today).
2. `html::parse` → DOM tree. Errors are recovered, never fatal (§5).
3. Collect `<script>` blocks → `luxscript` frontend compiles each to bytecode.
4. Run top-level code of each script (M2). The VM's only natives are the
   browser ones (§4.3); anything else is a compile-time unknown / run-time
   error, never a silent escape hatch.
5. Style (M3), layout, paint (M1 onwards, text only at first).

## 3. Component map

```
src/
  dom/node.hpp/.cpp      DOM: Node (Document/Doctype/Element/Text/Comment),
                         tree helpers, debug dump
  html/tokenizer.*       bytes → tags/text/comments (raw-text mode for
                         <script>/<style>)
  html/parser.*          tokens → DOM tree with implied html/head/body,
                         void elements, implied end tags, recovery
  luxscript/             Lux Script frontend skeleton (lexer today). In M2 this
                         directory becomes the vendored Lux frontend:
                         lexer/parser/ast/ir/emitter/bytecode/value/vm —
                         copied from ../Lux, namespace untouched
                         (lux_script::*), wrapped by luxium::script::Engine
  main.cpp               `luxium file.html` CLI: parse, dump tree, lex scripts
```

Rule of the codebase so far: every layer is a pure function of its input plus
explicit context — no globals, no hidden I/O. The shell (when it exists) drives
everything; layers never call upward.

## 4. The LuxScript engine

### 4.1 Why vendor Lux's engine instead of writing a new one

Lux Script already exists, is documented to zero ambiguity
(`../Lux/LUX_SCRIPT-GRAMMAR.md`), is statically typed with compile-time method
resolution, and already compiles to bytecode for a suspendable VM. A browser
needs exactly that: **runtime compilation** (the `--native` AOT path stays
server-side; the browser always interprets bytecode). Duplicating the language
would fork the grammar on day one. So: the frontend and VM are *copied* from
Lux at pinned commits (a real copy into `src/luxscript/`, not a symlink — the
browser will drift on purpose, see §4.3), built as a static library
`libluxscript`, and Luxium speaks to it through one thin class:

```cpp
namespace luxium::script {
class Engine {
    // compile: source text → bytecode chunk (+ diagnostics with line/col)
    // run:     chunk + natives → Value | error, with a per-page VM
};
}
```

### 4.2 What gets vendored, what does not

| From Lux | Into Luxium | Why |
|---|---|---|
| `token, lexer, ast, parser` | ✅ | language frontend |
| `ir, emitter, bytecode`     | ✅ | compilation to bytecode |
| `value, vm, diagnostic, type` | ✅ | execution + errors |
| `tiny_regex`                | ✅ (if the frontend depends on it) | vendored dependency of the above |
| `project, template, autotest` | later, maybe | multi-file projects, templating — page model first |
| `db*, auth, crypto, sqlite_replica` | ❌ never | server capabilities |
| `native_*` (AOT to C++)     | ❌ | AOT makes no sense at page-load time |

### 4.3 Natives: the whole security model in one table

The server VM reaches the world through native tables (`kNatives`,
`BuiltinModuleRegistry`). Luxium defines **its own** tables; a script can only
do what these tables expose, because the VM has no other I/O. This is the
sandbox: not a policy, a smaller API surface.

| Native surface | Exposes | Since |
|---|---|---|
| `log` / console | `log.info/warn/error` → devtools console | M2 |
| `document` (read) | `document.title`, node counts, `document.get(id)`, tag queries → read-only handles | M2 |
| `document` (write) | `element.set_text`, create/remove nodes, attribute edits | M4 |
| `events` | `element.on("click", fn)` handlers, `page.after(ms, fn)` timers | M4 |
| `net` (fetch) | http GET → string, **capability-gated per origin** | M5+ |
| fs, process, db | ❌ never exposed | — |

Object handles (elements) are opaque native classes: the script holds a
reference, the browser owns the node. A stale handle (node removed) fails
loudly, never crashes the browser.

### 4.4 Execution model

- One `Engine` (compile cache) and one **VM per page**, like Lux's one-VM-per-
  request: no shared mutable state between pages, nothing to lock.
- Scripts compile during parse (step 3 above); top-level runs once the DOM for
  the whole document exists (`document` is complete before top-level code
  runs — Luxium does **not** do incremental parsing like browsers do; it is a
  deliberate simplification).
- The VM's suspend/resume design (bytecode-operand stack off the C++ stack)
  is what M4's event loop hooks into: a click handler is just `start()` on a
  chunk; a timer or fetch suspends the VM exactly like a server handler
  suspends on `await`.
- Long-running top-level code: M2 guards with an instruction budget per run;
  exceeding it kills the script with a diagnostic (infinite loops must not
  freeze the UI thread).

## 5. HTML parsing policy

Full WHATWG tree construction is thousands of edge cases tuned for
compatibility with 1998 pages. Luxium targets **well-formed, modern,
hand-written HTML** and recovers instead of reproducing every legacy quirk:

Implemented in M0: raw-text mode for `script`/`style`; implied `html`/`head`/
`body`; void elements; `</br>` → `<br>`; stray end tags dropped with a
diagnostic; basic implied end tags (`<li>`, `<p>` vs. block starts, table rows
top-level only).

Not implemented (yet or ever): foster parenting, adoption agency algorithm,
full table scope, foreign content (SVG/MathML), template element, encoding
sniffing (UTF-8 only), entity tables beyond the trivial ones.

Every recovery records a diagnostic with line number. Goal: a page either
renders as its author intended, or the devtools say exactly what was dropped.

## 6. Rendering pipeline (M1+)

Software first, GPU maybe later:

```
DOM → (M3: matched styles) → box tree → layout → display list → raster
```

- **Boxes**: block and inline only at first; text shaping via system fonts
  (FreeType/HarfBuzz decision deferred to M1 start — a `wcwidth`-grade
  approximation is acceptable for M1's "text on screen" goal).
- **Raster**: CPU framebuffer (argb), blitted to the window by the shell. No
  compositing threads until something measurable demands them.
- **Damage tracking**: full-screen repaint is fine for M1; the display list
  exists from day one so dirty-rect repainting is a local change later.

## 7. Concurrency model

One UI thread owns: DOM, CSSOM, box tree, framebuffer, and every VM. Scripts
run on the UI thread (guarded by the instruction budget, §4.4). Networking
(M5) does I/O on worker threads and delivers completed bytes as events into
the UI thread's queue — the same shape as Lux's event loop, inverted.

## 8. Decisions taken

| Question | Decision | Why |
|---|---|---|
| Script embedding syntax | plain `<script>` is LuxScript; `type="text/javascript"` is refused with a console message | there is no other language to disambiguate from — see [LUXSCRIPT.md §2](LUXSCRIPT.md) |
| Shell toolkit | **SDL2** window + own software framebuffer | installed and smallest; GTK3 was the fallback and remains one |
| Text stack | **FreeType + HarfBuzz**, DejaVu Sans regular+bold, glyph cache keyed by (glyph, bold) | shaping control; HarfBuzz reuses FreeType's faces (hb_ft) |
| Handler state | `page.get/page.set` page-scoped store | a script's top level is a fn body: its locals die before a click can arrive. This is the browser's `state.*` |
| CSS scope for M3 | tag/class/id + box model + color/font | enough to lay out a form |
| C++ JSON for devtools | own tiny writer, like Lux's | no deps |

One deliberate divergence from the vendored engine: `render()`/templates are
stubbed to a compile-time diagnostic — a page renders its own DOM, it does
not render templates.

Two debug envs exist for the pipelines: `LUXIUM_DEBUG_WRAP` (dumps the
wrapped script source the compiler sees) and `LUXIUM_DEBUG_LAYOUT` (dumps the
box list). They are how the wrapping and layout bugs were found and how the
next ones will be.
