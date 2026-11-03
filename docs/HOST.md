# App host (`luxium --host`)

One Luxium browser process serves every desktop app that wants a window
(Lux Desktop on Linux). One Chromium instance, one chrome-less SDL window per
app, one private persistent profile per app.

```
luxium --host
  $XDG_RUNTIME_DIR/luxium/        0700   (fallback /tmp/luxium-<uid>)
    host.sock                     0600   unix stream socket
    host.lock                            flock held by the host for its whole life
    spawn.lock                           flock clients take while starting a host
    host.log                             host stdout/stderr when a client spawned it
```

## Lifecycle

- A client connects to `host.sock`. If nothing answers it takes `spawn.lock`,
  retries, then starts `luxium --host` detached (double fork + setsid) and
  polls the socket. Two apps starting at once end with one host: the spawn lock
  serializes them, and the host itself takes `host.lock` (a second host waits
  up to 20 s, then exits), so a stale socket file is always safe to replace.
- One connection = one window. Dropping the connection closes the window.
- The host exits `LUXIUM_HOST_GRACE` seconds (default 3) after the last window
  and connection are gone. It stops listening before it shuts Chromium down.
- A client that sees EOF before `opened` should reconnect (the host was exiting).

## Protocol

One JSON object per line, both directions. Values are scalars.

Client to host:

| cmd | fields |
|---|---|
| `open` | `url`, `title`, `width`, `height`, `min_width`, `min_height`, `resizable`, `dark`, `devtools`, `icon` (absolute path; PNG alpha is kept), `app_id` (profile key: `<profile root>/<app_id>`), `wm_class` (X11 WM_CLASS name and class, default `app_id`; match it to the `.desktop` file's `StartupWMClass`) |
| `navigate` | `url` |
| `reload`, `close`, `minimize`, `maximize`, `restore`, `devtools` | |
| `set_title` | `title` |
| `resize` | `width`, `height` |
| `fullscreen`, `always_on_top` | `on` |
| `eval` | `code` (JavaScript, fire and forget) |
| `post_message` | `data` (string; page gets a `luxium` CustomEvent, `event.detail`) |
| `set_menu` | `spec` (string: JSON list as in Lux's `window.set_menu()`; `[]` removes the bar) |
| `set_dark` | `on` (dark title bar hint and menu colours) |
| `show`, `hide`, `toggle` | (`toggle`: hide a visible window, show and raise a hidden one; for tray clicks) |
| `clipboard_write` | `text` |
| `clipboard_read` | `id` (answered by a `result` event) |
| `ping` | |

Host to client: `opened {width,height}`, `resized {width,height}`,
`title {title}` (page title, informational), `url {url}`, `loaded`,
`message {data}` (page called `console.debug("luxium-msg:<data>")`),
`menu {action[,checked]}` (a menu item was activated; `checked` = new state, only for check items), `result {id,value}`, `closed` (window gone; the host then drops the connection),
`error {message}`, `pong`.

## Windows

- Page fills the whole window; no URL bar, status line or console.
- `devtools:true` enables F12 / Ctrl+Shift+I (inspector in a second window) and F5 / Ctrl+R reload.
- Ctrl+C/X/V/A work as in the browser; Esc leaves fullscreen.
- Each `app_id` has its own request context (cookies, localStorage, cache) under
  `~/.local/share/luxium/apps/<app_id>`, separate from the normal browser's profile.
- WM_CLASS is the `app_id`, so desktop shells group and pin apps separately.

## Menu bar

`set_menu` draws a menu bar (26 px) above the page, in the window's own framebuffer: SDL has no native menus
and the page is rendered off-screen, so the bar and its drop-downs are painted by the host (dark or light
from `dark`; DejaVu Sans). Nothing in the GTK or X11 stack is involved, which keeps it independent of the
desktop and of CEF. The page view shrinks by the bar height.

Spec: `[{label, items:[...]}]`; an item is `{label, action, accel?, checked?}`, `{label, items:[...]}` (submenu,
any depth) or the string `"-"` (separator). `accel` uses GTK syntax (`<Control>q`, `<Control><Shift>s`, `F5`).
Accelerators are matched before keys are forwarded to the page, so they work while the page has focus. Activating
a leaf (click, Enter, accelerator) sends `menu`; a check item toggles itself first and reports the new state,
as the WebKitGTK backend does. Keyboard: F10 opens the bar, arrows move, Enter activates, Esc closes.
No mnemonics (Alt+letter).

## Window hints

Dark: `_GTK_THEME_VARIANT=dark` is set on the X11 window (the host always runs on X11, XWayland included; a
native Wayland session is not supported). Icon: sent as ARGB `_NET_WM_ICON`.

## Install

`tools/install.sh` (no sudo) copies the binary and the Chromium runtime to
`~/.local/opt/luxium` and links `~/.local/bin/luxium`.
