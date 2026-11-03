# Plan: real SPAs in LuxScript on Luxium's own engine (Homeflix as the bar)

Decision (user, 2026-11-03): Homeflix's frontend is ported **as a full SPA to
LuxScript** and runs on Luxium's **own engine**. **Lux-Local app windows do not
use CEF**: they render with the own engine; CEF stays only for JavaScript sites
in the normal Luxium browser and is started lazily, never for app windows.
Requirement kept from before: every container/codec plays directly through the
native media engine (no remux/transcode).

Reference app: `/home/burnt/Documentos/Github/Homeflix-Local/app/public`
(`js/api.js`, `router.js`, `app.js`, `views/*.js`, `css/style.css`,
`index.html`). Everything it does must be expressible and must work.

## 0. Ground rules (all workstreams)

- The language does not fork (docs/LUXSCRIPT.md §1): language/checker changes
  and `browser_api.inc` declarations land in **Lux, branch `luxium`**
  (`/home/burnt/Documentos/Github/Lux`) first; Luxium vendors them with
  `tools/sync_from_lux.sh`. Natives are Luxium's own (`src/lux_script/natives.cpp`,
  `src/script/engine.cpp`). Lux's 394 tests must stay green.
- No commit attribution to Claude/AI. English comments. Match the style.
- Test only on Xvfb `DISPLAY=:99` / SDL dummy drivers. `:1` is the user's real
  desktop: never open windows or inject input there; no screenshots of it.
  Luxium's own frame dumps (`--shot`, `--wait`, `--click-at`) are fine.
- Keep `css-tests` baseline (113/113) and `tools/mediatest.py` green.

## 1. API contract (workstream A implements, everyone codes against it)

Names are a starting point; A may adjust and must document the final surface in
docs/LUXSCRIPT.md (§3 tables) — other workstreams follow that doc.

**Async.** Event handlers, timers and the top level run as VM coroutines, so
`await` is valid there (today only route handlers). Suspendable in the browser:
`http.*`, `ws.recv()`, `sleep(ms)`, `page.frame()`.

**No closures in the language.** Handlers are named `fn`s; context comes from
`event` (target, key, value…) and from element data (`e.data("id")`). If a
closure/lambda feature is truly needed, propose it in Lux's grammar (A decides),
don't hack it into Luxium.

- `http.get/post/put/patch/delete(url, body?, headers?)` → `Response`
  (`status`, `ok`, `text()`, `json()`, `header(name)`), same-origin by default;
  relative URLs resolve against the page. Same surface as server-side `http`.
  Upload: `http.post(url, form)` with a `FormData`-like from a file input.
- `json.parse(string) -> Json`, `json.stringify(Json) -> string` (if not already).
- `ws.connect(url) -> Socket` with `send(string)`, `await recv()`, `close()`,
  plus an `on_message(fn)` style handler for the non-await pattern.
- `storage.get/set/remove(key)` — persistent per origin (localStorage
  equivalent), stored under the profile dir.
- `history.push(path)`, `history.replace(path)`, `history.back()`,
  `page.location()` (path, query → Dict), `page.on_navigate(fn)` (popstate).
  `<a href>` with a page-level hook to intercept same-origin links (SPA router).
- Events: `e.on(kind, fn)` is impossible (`on` is a keyword) — use per-kind
  methods or `e.listen("click", fn)`; kinds: click, dblclick, input, change,
  submit, keydown, keyup, focus, blur, mouseenter, mouseleave, mousemove,
  scroll, ended/timeupdate/… for media. Document-level `document.listen`.
  `event.target()`, `event.key()`, `event.value()`, `event.prevent()`,
  `event.stop()`, modifiers, mouse x/y.
- Elements: `query(sel)`, `query_all(sel)` scoped, `closest(sel)`, `parent()`,
  `children()`, `set_html(string)` (parsed + scripts NOT executed),
  `html()`, `value()/set_value()`, `checked()/set_checked()`, `disabled`,
  `add_class/remove_class/toggle_class/has_class`, `set_style(prop, value)`,
  `style(prop)`, `data(name)`, `focus()`, `blur()`, `rect()`,
  `scroll_into_view()`, `scroll_top/set_scroll_top`, `insert_before`,
  `replace_with`, `clone()`.
- Templates: `render(template_string, Dict) -> string` with Lux's template
  engine (already vendored: src/lux_script/template.cpp), so views build HTML
  the way Homeflix builds template literals.
- Timers: `page.after(ms, fn) -> int`, `page.every(ms, fn) -> int`,
  `page.cancel(id)`.
- Media element API: `play()`, `pause()`, `seek(t)`, `current_time()`,
  `duration()`, `paused()`, `ended()`, `set_volume`, `set_muted`,
  `audio_tracks()`, `select_audio(i)`, `text_tracks()`, `select_text(i)`,
  `set_src(url)`, events via `listen`. Backed by `media::own` (see
  src/media/own_media.hpp).
- `document.fullscreen(e)`, `document.exit_fullscreen()`,
  `document.fullscreen_element()`; `clipboard.write(string)`;
  `page.title(string)`.
- Errors in handlers go to the console and never kill the page.

## 2. Workstreams (parallel; each its own worktree/branch, coordinator merges)

**A — Language + runtime API** (Lux `luxium` branch + Luxium worktree
`Luxium-api`): everything in §1: checker/VM changes for browser coroutines,
`browser_api.inc` declarations, Luxium natives (curl worker threads for http
with coroutine resume on the main loop; websocket client via libcurl ws or a
small own client; storage; history; events dispatch; element API; timers;
media API glue). Tests: headless pages in `examples/spa/` driven by
`luxium_headless` + `--click-at`, a local test server (python) for http/ws.

**B — Engine: forms, input and CSS gaps** (worktree `Luxium-engine`):
`<input>` (text, password, search, number, range, checkbox, radio, file →
zenity dialog), `<select>` (dropdown popup), `<textarea>`, `<label>`, focus
ring/tab order, caret + selection + clipboard + IME text input, `:hover`,
`:focus`, `:focus-visible`, `:checked`, `:disabled`, the `cursor` property;
CSS `transform` (2D), `transition` for opacity/transform/colors (simple),
`position: sticky`, `overflow: auto/scroll` with internal scrolling + wheel,
`min()/max()/clamp()` mixing units, `outline`, `text-overflow: ellipsis`,
`aspect-ratio`, `object-fit` (img), `backdrop`-free overlays, `z-index`
stacking with fixed overlays; incremental restyle/relayout so DOM mutations
from scripts stay fast (measure: a 500-card catalog re-render < 16 ms).
Drive the list by rendering Homeflix's `css/style.css` + its markup (static
HTML snapshots of each view) and fixing what renders wrong — compare against
Chromium with tools/cssrun.py where possible.

**C — Lux-Local host without CEF** (Luxium master/`Luxium-host` worktree +
Lux-Local): the app host renders app windows with the own engine
(Shell-per-window), CEF never initialized unless a JS page is opened in the
normal browser. Keep menus/tray/dark/icon/WM_CLASS (src/host/*), the socket
protocol (docs/HOST.md; `eval` becomes LuxScript or is dropped), dev reload.
Lux-Local apps declare their frontend; JS-frontend apps (Localify, Mail…) keep
working — decide: CEF-backed host window only for those (lazy), or WebKitGTK
fallback; document. Measure PSS for 1/2/4 own-engine apps (target: well below
WebKitGTK's ~313 MB for two apps).

**D — Homeflix port** (Homeflix-Local `app/public`, after A+B have the basics;
may start with catalog/series views as soon as http + set_html + events work):
translate `api.js`, `router.js`, `app.js`, every view to LuxScript; playback
always direct through the media API (no remux/transcode in Luxium); "ver
juntos" via ws; downloads view via ws; uploads via file input + http.post.
Keep server routes unchanged unless a route only existed for a JS-only reason.
Don't commit Homeflix-Local (it has no commits yet) unless the user asks;
reinstall via its `.run` with pkexec at the end.

**E — Media engine formats** (already running, worktree `Luxium-direct`):
every container/codec direct, in-file audio track switching, embedded text
subtitles, own-engine wiring.

## 3. Acceptance

Homeflix-Local opens in a Lux-Local window rendered by the own engine (no CEF
process in the tree), every view works (profile picker, catalog, series,
player with direct playback of MKV/HEVC/AC3, audio/subtitle switch, fullscreen,
progress reporting, downloads, uploads, ver juntos), and memory is measured.
