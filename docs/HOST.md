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
| `open` | `url`, `title`, `width`, `height`, `min_width`, `min_height`, `resizable`, `dark`, `devtools`, `icon` (absolute path), `app_id` (profile key: `<profile root>/<app_id>`) |
| `navigate` | `url` |
| `reload`, `close`, `minimize`, `maximize`, `restore`, `devtools` | |
| `set_title` | `title` |
| `resize` | `width`, `height` |
| `fullscreen`, `always_on_top` | `on` |
| `eval` | `code` (JavaScript, fire and forget) |
| `post_message` | `data` (string; page gets a `luxium` CustomEvent, `event.detail`) |
| `clipboard_write` | `text` |
| `clipboard_read` | `id` (answered by a `result` event) |
| `ping` | |

Host to client: `opened {width,height}`, `resized {width,height}`,
`title {title}` (page title, informational), `url {url}`, `loaded`,
`message {data}` (page called `console.debug("luxium-msg:<data>")`),
`result {id,value}`, `closed` (window gone; the host then drops the connection),
`error {message}`, `pong`.

## Windows

- Page fills the whole window; no URL bar, status line or console.
- `devtools:true` enables F12 / Ctrl+Shift+I (inspector in a second window) and F5 / Ctrl+R reload.
- Ctrl+C/X/V/A work as in the browser; Esc leaves fullscreen.
- Each `app_id` has its own request context (cookies, localStorage, cache) under
  `~/.local/share/luxium/apps/<app_id>`, separate from the normal browser's profile.
- WM_CLASS is the `app_id`, so desktop shells group and pin apps separately.

## Install

`tools/install.sh` (no sudo) copies the binary and the Chromium runtime to
`~/.local/opt/luxium` and links `~/.local/bin/luxium`.
