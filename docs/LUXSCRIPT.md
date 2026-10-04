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
| `document` | top level + any handler | — (new) | M2 |
| `element` handles | returned by `document.get/query` | — (new) | M2 |
| `page` | top level + any handler | — (new) | M4 |
| `event` | inside an event handler | — (new) | M4 |

Sketch of the M2 surface (read-only):

```lux
# runs once, after the DOM is complete
document.title = document.title + " — Luxium"      # reads; writes come in M4
Element root = document.root()
log.info("nodes: " + str(root.child_count()))

Element status = document.get("status")            # by id; null if absent
if status != null:
    log.info("status text: " + status.text())
```

The full per-context tables will be documented here as each milestone lands
them, in the same style as the grammar's §45 ("the complete surface").

## 4. Compilation and execution

- **When:** each `<script>` compiles during the page's parse phase; bytecode
  is cached on the script node. Top-level code runs once, after the *whole*
  DOM exists (Luxium parses incrementally-free by design — see
  [ARCHITECTURE §4.4](ARCHITECTURE.md#44-execution-model)).
- **Where:** one VM per page; scripts of one page share that VM's globals in
  document order, so a `fn` defined in the first `<script>` is visible to the
  second — same-file semantics as a Lux server project's module.
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
