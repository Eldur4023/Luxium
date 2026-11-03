# LuxScript in the browser

> How Lux Script — *the same language* Lux the server compiles
> ([grammar reference](../Lux/LUX_SCRIPT-GRAMMAR.md)) — embeds in, compiles
> for, and executes inside a Luxium page. This document only covers the
> **differences and the embedding**; it never redefines the language.

---

## 1. The language does not fork

Everything in LUX_SCRIPT-GRAMMAR.md holds verbatim in Luxium: indentation
blocks, static typing with bidirectional inference, compile-time method
resolution, `#` comments, no `;`, the same precedence table, the same
diagnostics philosophy (errors are precise and quoted). If Luxium ever needs a
construct the server lacks, it lands in Lux's grammar first and Luxium vendors
the update — one language, two hosts.

## 2. Embedding a script in a page

**Decision (leaning, open to change until M2):** a plain `<script>` tag is
LuxScript. There is no other scripting language to disambiguate from, so the
attribute ceremony is noise:

```html
<script>
int clicks = 0
log.info("page ready")
</script>
```

- `<script type="text/luxscript">` is accepted and identical (so pages can be
  explicit, and so tooling that keys off `type` stays happy).
- `<script src="page.lux">` loads an external file (M2: relative to the page).
- A future `<script type="text/javascript">` is a **diagnostic**, not a
  feature: "Luxium does not run JavaScript" — loud, in the console, once per
  page. A browser that silently ignores scripts teaches the wrong lesson.
- `<noscript>`-style fallback tags are unnecessary: there is no "off" state.

Open sub-question kept until M2: a dedicated `<lux>` tag reads better in
markup-heavy documents but breaks every HTML highlighter; `src=` filenames get
the `.lux` extension either way.

## 3. What replaces the reserved objects

The language's reserved objects exist only in the context that defines them
(grammar §2). The **server** contexts (`app`, routes, handlers, `request`,
`response`) do not exist in a browser. Luxium defines its own contexts:

| Reserved object | Available in | Replaces (server) | Since |
|---|---|---|---|
| `log` | anywhere | `log` (kept) | M2 |
| `document` | anywhere | — (new) | M2 |
| `element` handles | returned by `document.get/query_all/root` | — (new) | M2 |
| `page` | anywhere | `state` (the page-scoped store) | M2 |
| `page.after(ms, fn)` / `page.clicked()` | timers + handler context | — (new) | M4 |
| `event` | inside an event handler | `page.clicked()` covers the target today | M4+ |

DOM mutation (M4), as shipped:

```lux
Element li = document.create("li")     # detached node
li.set_attr("id", "tarea3")
li.set_text("compra pan")
li.click(borra)                        # handlers on created nodes work
document.get("lista").append(li)       # attach; relayout happens by itself

e.remove()                             # subtree gone, handles die loudly
```

Timers, frame-driven (one shot; re-arm inside the handler):

```lux
fn void reloj():
    document.get("reloj").set_text(str(page.get("n")))
    page.after(400, reloj)

page.after(400, reloj)
```

Inside a click handler, `page.clicked()` is the element that received the
event (the minimal `event` of M4; no closures, so this is how a handler
knows its target).

The M2 surface, as shipped:

```lux
# top level: runs once, after the DOM is complete
log.info("titulo: " + document.title())

Element root = document.root()
log.info("hijos: " + str(root.child_count()))

Element status = document.get("status")      # by id; null if absent
if status != null:
    log.info("status: " + status.text())

for Element p in document.query_all("p"):    # typed loop variable
    log.info(p.tag() + ": " + p.text())

# handler state MUST live in the page store: the top level is a fn body,
# so its locals die before any click can arrive
page.set("n", 0)

fn void suma():
    int n = int(page.get("n")) + 1
    page.set("n", n)
    document.get("out").set_text(str(n))

document.get("mas").click(suma)              # NOT .on("click", fn):
                                             # `on` is a language keyword

### 3.1 The SPA surface (workstream A, `docs/plan-luxscript-spa.md` §1)

The authoritative list is `Lux/include/lux_script/browser_api.inc` (the checker
compiles against it; Luxium implements each entry in `src/script/engine.cpp`).
Status column: `done` = implemented and tested; `wip` = declared, being built.

**Execution model.** The top level, every event handler, every timer callback
and every websocket callback run as VM coroutines on the main loop, so `await`
is valid in any `fn`. Suspendable: `sleep(ms)`, `page.frame()` (resumes next
frame), `http.*`, `await sock.recv()`, `await sock.wait_open()`. A runtime
error inside a handler/callback is logged to the console and never stops the
page. A handler is a named `fn` taking **no parameters** — the event is read
through the reserved object `event` (`event.key()`, `event.target()`...).
Context between handlers lives in the page store (`page.get/set`), in element
data (`e.data("id")`) or in the DOM. There are no closures; a closure feature
was not needed to write Homeflix.

**Reserved objects** (`status`: all `wip` until the commit that says otherwise)

| Object | Members |
|---|---|
| `log` | `info(s)` `warn(s)` `error(s)` |
| `document` | `get(id)` `query(sel)` `query_all(sel)` `root()` `body()` `create(tag)` `title()` `listen(kind, fn [, capture])` `unlisten(kind, fn)` `fullscreen(e)` `exit_fullscreen()` `fullscreen_element()` `active_element()` `hidden()` |
| `page` | `get(k)` `set(k, v)` `after(ms, fn) -> int` `every(ms, fn) -> int` `cancel(id)` `await frame()` `now()` (monotonic ms) `location() -> Dict{url, origin, path, query: Dict, search, hash}` `on_navigate(fn)` (back/forward) `listen(kind, fn)` (window: `resize`, `visibilitychange`, `fullscreenchange`) `title([s])` `scroll_y()` `scroll_to(y)` `reload()` `clicked()` |
| `event` | `kind()` `target()` `current()` `key()` `code()` `value()` `button()` `x()` `y()` `delta()` `ctrl()` `shift()` `alt()` `meta()` `repeat()` `prevent()` `stop()` `prevented()` `message()` (ws text) `close_code()` |
| `http` | `await get(url [, headers])` `await delete(url [, headers])` `await post(url, body [, headers])` `await put(...)` `await patch(...)` -> `Response`. Same origin only; relative URLs resolve against the page. `body`: string as-is, Dict/List -> JSON (`Content-Type` set), `Form` -> multipart. A network failure raises (catchable with `try`); an HTTP error status is a normal `Response` |
| `json` | `parse(s) -> Json` (raises on bad JSON) `stringify(v) -> string` |
| `ws` | `connect(url) -> Socket` (ws/wss, relative URLs resolve against the page) |
| `storage` | `get(k) -> string?` `set(k, s)` `remove(k)` `keys()` `clear()` — persistent per origin, `$LUXIUM_PROFILE_DIR` or `~/.local/share/luxium/storage/` |
| `history` | `push(path)` `replace(path)` `back()` `forward()` — no reload, no `on_navigate` for your own pushes |
| `clipboard` | `write(s)` `read() -> string` |
| `template` | `render(src, Dict) -> string` (Lux's template engine: `{{ x }}` escapes, `{% for %}`, `{% if %}`) `escape(s)` |
| `url` | `encode(s)` `decode(s)` `parse_query(s) -> Dict` |
| `math` | `floor` `ceil` `round` `abs` `min(a,b)` `max(a,b)` `fixed(x, digits) -> string` |
| `time` | `now()` (epoch ms) |
| globals | `sleep(ms)` (await) `form_data([formElement]) -> Form` plus `len str int float range` |

**Event kinds** for `listen`: `click dblclick mousedown mouseup mousemove mouseenter
mouseleave wheel keydown keyup input change submit focus blur scroll` on
elements and the document (they bubble; `capture=true` as 3rd argument listens in
the capture phase; `event.stop()` stops propagation; `event.prevent()` cancels the
default action: link navigation, form submit). Media elements add `play pause
ended timeupdate seeking seeked loadedmetadata durationchange volumechange error
waiting playing cuechange`. `<a href>`: after bubbling, if not prevented, Luxium
navigates; a SPA intercepts with a `document.listen("click", ...)` handler that
calls `event.prevent()` and `history.push(...)`.

**Types (receivers).**

`Element` — DOM: `text() set_text(s) html() set_html(s)` (parsed; `<script>` dropped)
`append(e) prepend(e) remove() insert_before(new, ref) replace_with(e) clone([deep])
attr(n) set_attr(n, v) remove_attr(n) has_attr(n) tag() id() child_count()
query(sel) query_all(sel) closest(sel) matches(sel) parent() children() next() prev()
contains(e) connected()` — style/state: `add_class remove_class toggle_class(n [, force])
has_class set_style(prop, v) style(prop) data(n) set_data(n, v) hidden() set_hidden(b)` —
forms: `value() set_value(s) selected_text() checked() set_checked(b) disabled()
set_disabled(b) files() -> List<File> show_modal() close([value]) return_value()` —
events/geometry: `click(fn) listen(kind, fn [, capture]) unlisten(kind, fn) dispatch(kind)
focus() blur() rect() -> Dict{x,y,w,h} scroll_into_view() scroll_top() set_scroll_top(f)
scroll_left() set_scroll_left(f)` — media (`<video>`/`<audio>`, backed by `media::own`):
`play() pause() seek(t) current_time() duration() paused() ended() seeking() set_volume(v)
volume() set_muted(b) muted() audio_tracks() -> List<Dict{index,label,current}>
select_audio(i) text_tracks() select_text(i) cue() -> string set_src(url) src()`
(`select_text(-1)` = off). Queries return `null` when nothing matches.

`Response`: `status() ok() text() json() header(name) url()`.
`Socket`: `send(s) close([code]) is_open() on_open(fn) on_message(fn) on_close(fn)`
`await recv() -> string?` (`null` once closed and drained) `await wait_open() -> bool`.
In `on_*` callbacks `event.message()` / `event.close_code()` carry the data.
`Form`: `add(name, value) add_file(name, File)`. `File`: `name() size() path() content_type()`.

**Host hooks (for B/C/shell).** The engine reaches layout, focus, scroll,
fullscreen, clipboard and media through `page::Page`'s hook structs
(`src/page/page.hpp`: `ShellHooks`, `MediaApi`); each is optional and has a
no-layout fallback so the headless harness runs everything except geometry.
Input is delivered with `Page::dispatch(kind, node, EventData)`.


## 4. Compilation and execution

- **When:** each `<script>` compiles during the page's parse phase; bytecode
  is cached on the script node. Top-level code runs once, after the *whole*
  DOM exists (Luxium parses incrementally-free by design — see
  [ARCHITECTURE §4.4](ARCHITECTURE.md#44-execution-model)).
- **Where:** all of a page's `<script>` blocks compile as ONE program:
  statements run inside one `__luxium_main__` in document order, and
  `fn`/`class`/`enum` declarations are hoisted to the program's top level
  (the parser rejects them nested) — so a `fn` defined in the first
  `<script>` is visible to the second, and both see the same globals.
  Compile errors report with the line **of the HTML document**, not of the
  invisible wrapped source.
- **Errors:** compile errors report with the script's line/col *remapped to
  the HTML document's* coordinates (the engine prepends the script's source
  offset to every diagnostic), and appear in the console pane. The page still
  renders its HTML; a broken script never blanks a page.
- **The budget:** every top-level run and every handler invocation has an
  instruction budget (value TBD by profiling M2). Exhaustion kills that run
  with `error: instruction budget exceeded at line N`. The page keeps
  rendering; the rest of its scripts still run.

## 5. Sandboxing

Lux Script on the server reaches the world only through natives. Luxium's VM
exposes fewer:

- No `db`, no `crypto`, no `auth`, no modules that touch the filesystem.
- No dynamic `import` of arbitrary paths (external scripts come through
  `<script src>`, subject to the same page-origin rules as the page).
- `net` (M5+) is a capability: pages from `file://` get no network by default;
  http pages get same-origin GET, asked for explicitly. Every other native
  table simply does not exist in the VM — the sandbox is the API surface,
  not a checker.
- Elements are opaque handles; scripts cannot forge one, and a stale handle
  (node removed) fails loudly.

## 6. A complete M2-shaped example

```html
<!doctype html>
<html>
<head>
    <title>Contador</title>
</head>
<body>
    <h1 id="titulo">Demo</h1>
    <p id="status">hola</p>

    <script>
    # top level: runs once, DOM complete
    int base = 41
    log.info("base = " + str(base))

    fn duplica(int x) -> int:
        return x * 2

    Element status = document.get("status")
    log.info("status: " + status.text())
    log.info("duplica(base) = " + str(duplica(base)))
    </script>
</body>
</html>
```

(M4 adds the click handler that makes *Contador* an actual counter — that
page is the M4 acceptance test.)
