#!/usr/bin/env python3
"""Modern-web parity checks for Luxium's CEF side, with REAL input: events are
injected with xdotool into an X display (use Xvfb: DISPLAY=:99) and travel the
same path as a user's -- SDL -> shell -> CefBrowserHost -- so key/mouse/wheel/
cursor/dialog/download/clipboard wiring is covered, not just Chromium itself.

    DISPLAY=:99 tools/paritytest.py

zenity is shadowed by a stub in PATH (dialogs would otherwise wait for a human);
the stub records its arguments.
"""
import base64, hashlib, http.server, os, subprocess, sys, tempfile, threading, time, json
sys.path.insert(0, os.path.dirname(__file__))
from cdp import Page

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PORT, CDP = 8791, 9355
W = os.environ.get("DISPLAY", ":99")
tmp = tempfile.mkdtemp(prefix="luxium-parity-")
prof, dl, binp = f"{tmp}/profile", f"{tmp}/dl", f"{tmp}/bin"
os.makedirs(binp)
for n in ("picked1.txt", "picked2.txt"):
    open(f"{tmp}/{n}", "w").write(n)
open(f"{binp}/zenity", "w").write(f"""#!/bin/bash
echo "$@" >> {tmp}/zenity.log
case "$*" in
  *--file-selection*) printf '{tmp}/picked1.txt\\n{tmp}/picked2.txt\\n';;
  *--entry*) echo luxium;;
  *) exit 0;;
esac
""")
os.chmod(f"{binp}/zenity", 0o755)
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(("  ok   " if ok else "  FAIL ") + name + (f"  [{detail}]" if detail != "" else ""))


def xdo(*a):
    subprocess.run(["xdotool", *a], env=dict(os.environ, DISPLAY=W), check=True)


def launch(url, **extra):
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", LUXIUM_CDP_PORT=str(CDP), LUXIUM_PROFILE=prof,
               XDG_DOWNLOAD_DIR=dl, PATH=binp + ":" + os.environ["PATH"], LUXIUM_DEBUG_CEF="1", DISPLAY=W, **extra)
    log = open(f"{tmp}/run.log", "ab")
    global PROC
    PROC = subprocess.Popen([os.path.join(ROOT, "build/luxium"), url], env=env, cwd=os.path.join(ROOT, "build"),
                            stdout=log, stderr=log, start_new_session=True)
    return PROC


def stop(proc):
    proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()
    os.system("pkill -x luxium 2>/dev/null")
    time.sleep(1.5)


def click_el(p, sel, dbl=False):
    r = p.eval(f"(() => {{ const r = document.querySelector('{sel}').getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2] }})()")
    win = subprocess.check_output(["xdotool", "search", "--pid", str(PROC.pid)], env=dict(os.environ, DISPLAY=W)).split()[0].decode()
    xdo("mousemove", "--window", win, str(int(r[0])), str(int(r[1]) + 38))   # +38: the address strip
    time.sleep(0.2)
    xdo("click", "--repeat", "2" if dbl else "1", "1")
    time.sleep(0.5)


class Net(http.server.BaseHTTPRequestHandler):
    """fetch (JSON), EventSource (3 events) and a WebSocket echo, all on one port."""
    def log_message(self, *a): pass

    def do_GET(self):
        if self.headers.get("Upgrade", "").lower() == "websocket":
            acc = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.wfile.write(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {acc}\r\n\r\n".encode())
            h = self.rfile.read(2)
            n = h[1] & 0x7F
            mask = self.rfile.read(4)
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(n)))
            self.wfile.write(bytes([0x81, len(data)]) + data)
            self.wfile.flush()
            return
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        if self.path == "/sse":
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for i in range(3):
                self.wfile.write(f"data: tick{i}\n\n".encode()); self.wfile.flush(); time.sleep(0.1)
        else:
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok": true}')


def main():
    net = http.server.ThreadingHTTPServer(("127.0.0.1", PORT + 1), Net)
    threading.Thread(target=net.serve_forever, daemon=True).start()
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools/rangeserver.py"), str(PORT), os.path.join(ROOT, "examples")])
    time.sleep(0.5)
    url = f"http://127.0.0.1:{PORT}/parity.html"
    proc = launch(url)
    try:
        p = Page(CDP, wait=30)
        p.wait_for("document.readyState === 'complete'", 20)
        time.sleep(1)
        # --- persistence (part 1): written now, read after a restart
        p.eval("localStorage.setItem('lx', 'persisted'); document.cookie = 'lxc=cookie; max-age=3600; path=/'")
        # --- file dialog
        click_el(p, "#file")
        p.wait_for("window.__log && window.__log.some(l => l[0] === 'files')", 10)
        check("<input type=file> opens a dialog and delivers the files",
              p.eval("window.__log.find(l => l[0] === 'files')[1].join()") == "picked1.txt,picked2.txt")
        # --- JS dialogs
        click_el(p, "#alert")
        p.wait_for("window.__log.some(l => l[0] === 'alert-returned')", 10)
        click_el(p, "#confirm")
        p.wait_for("window.__log.some(l => l[0] === 'confirm')", 10)
        click_el(p, "#prompt")
        p.wait_for("window.__log.some(l => l[0] === 'prompt')", 10)
        lg = p.eval("window.__log")
        check("alert returns", ["alert-returned"] in lg)
        check("confirm returns true on OK", ["confirm", True] in lg)
        check("prompt returns the typed text", ["prompt", "luxium"] in lg, json.dumps(lg[-1]))
        # --- keys: real key events reach the page
        click_el(p, "body")
        for k in ("f", "space", "Left", "Right", "Return"):
            xdo("key", k)
            time.sleep(0.1)
        keys = p.eval("window.__keys.filter(k => k[0] === 'down').map(k => k[1])")
        check("keyboard reaches the page", keys[-5:] == ["f", " ", "ArrowLeft", "ArrowRight", "Enter"], json.dumps(keys[-5:]))
        # --- wheel
        xdo("click", "5"); xdo("click", "5"); xdo("click", "4")
        time.sleep(0.4)
        check("mouse wheel reaches the page", p.eval("window.__wheel || 0") >= 2, p.eval("window.__wheel || 0"))
        # --- clipboard: Ctrl+C in an input, Ctrl+V in a textarea
        click_el(p, "#txt")
        xdo("key", "ctrl+a"); xdo("key", "ctrl+c"); time.sleep(0.3)
        click_el(p, "#ta")
        xdo("key", "ctrl+v"); time.sleep(0.4)
        check("copy/paste between fields", p.eval("document.getElementById('ta').value") == "clip me", p.eval("document.getElementById('ta').value"))
        ok = p.eval("navigator.clipboard.writeText('via api').then(() => navigator.clipboard.readText())", gesture=True)
        check("navigator.clipboard write+read", ok == "via api", ok)
        # --- cursors
        click_el(p, "#pad")
        time.sleep(0.4)
        click_el(p, "#txt")
        time.sleep(0.4)
        cur = [l for l in open(f"{tmp}/run.log", errors="replace") if l.startswith("[cursor]")]
        check("cursor type follows the page (grab-ish / ibeam)", any("type 3" in l for l in cur), "".join(cur[-4:]).strip())
        # --- IME / non-ASCII text: committed text must arrive whole
        click_el(p, "#ta")
        xdo("key", "ctrl+a"); xdo("type", "--delay", "80", "ñé€")
        time.sleep(0.5)
        check("non-ASCII text input (accents, euro)", p.eval("document.getElementById('ta').value") == "ñé€", p.eval("document.getElementById('ta').value"))
        # --- fetch / EventSource / WebSocket
        base = f"127.0.0.1:{PORT + 1}"
        check("fetch (cross-origin JSON)", p.eval(f"fetch('http://{base}/json').then(r => r.json()).then(j => j.ok)") is True)
        check("EventSource receives events", p.eval(f"new Promise(res => {{ const got = []; const es = new EventSource('http://{base}/sse'); es.onmessage = e => {{ got.push(e.data); if (got.length == 3) {{ es.close(); res(got.join()) }} }} }})") == "tick0,tick1,tick2")
        check("WebSocket echo", p.eval(f"new Promise((res, rej) => {{ const w = new WebSocket('ws://{base}/'); w.onopen = () => w.send('ping'); w.onmessage = e => res(e.data); w.onerror = () => rej('ws error') }})") == "ping")
        # --- fullscreen: element request -> window follows, chrome hidden, Esc leaves
        p.eval("document.documentElement.requestFullscreen().then(() => true)", gesture=True)
        time.sleep(1.2)
        sw, sh = subprocess.check_output(["xdotool", "getdisplaygeometry"], env=dict(os.environ, DISPLAY=W)).split()
        inner = p.eval("[innerWidth, innerHeight, !!document.fullscreenElement]")
        check("element fullscreen fills the screen, no chrome", inner == [int(sw), int(sh), True], f"{inner} vs {sw}x{sh}")
        xdo("key", "Escape"); time.sleep(1.2)
        check("Esc leaves fullscreen", p.eval("!document.fullscreenElement") is True and p.eval("innerHeight") < int(sh), p.eval("[innerWidth, innerHeight]"))
        # --- download
        click_el(p, "#dl")
        time.sleep(1.5)
        f = f"{dl}/luxium-dl.txt"
        check("download lands in the XDG downloads dir", os.path.exists(f) and open(f).read() == "hello download", f)
        # --- target=_blank opens in the same window
        click_el(p, "#blank")
        time.sleep(1.5)
        check("target=_blank navigates this window", "popup=1" in p.eval("location.href"), p.eval("location.href"))
    finally:
        stop(proc)
    # --- persistence (part 2)
    proc = launch(url)
    try:
        p = Page(CDP, wait=30)
        p.wait_for("document.readyState === 'complete'", 20)
        check("localStorage survives a restart", p.eval("localStorage.getItem('lx')") == "persisted")
        check("cookie survives a restart", "lxc=cookie" in p.eval("document.cookie"), p.eval("document.cookie"))
    finally:
        stop(proc)
    # --- HiDPI: scale 2 is applied as page zoom (OSR ignores the device scale factor): CSS viewport halves
    proc = launch(url, LUXIUM_SCALE="2")
    try:
        p = Page(CDP, wait=30)
        p.wait_for("document.readyState === 'complete'", 20)
        p.wait_for("innerWidth < 700", 10)
        r = p.eval("[devicePixelRatio, innerWidth]")
        check("HiDPI scale 2 -> page laid out at 2x (via zoom)", 500 < r[1] < 700, r)
    finally:
        stop(proc)
        srv.terminate()
    print("FAILED: %d" % results.count(False) if False in results else "all checks passed")
    sys.exit(1 if False in results else 0)


if __name__ == "__main__":
    main()
