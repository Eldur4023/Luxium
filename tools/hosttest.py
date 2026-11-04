#!/usr/bin/env python3
"""Failure-mode tests for the app host (luxium --host) and the Lux-Local client (src/luxium_window.cpp).

Everything runs on Xvfb (DISPLAY :99 by default) with a private HOME/XDG_* under /tmp/hxt-*: the socket path
must stay under 108 bytes. Never touches the real desktop; only kills processes it started itself.

  tools/hosttest.py [--luxium build/luxium] [--app ../Lux-Local/build/app] [only-test-name ...]
"""
import argparse, glob, json, os, shutil, signal, socket, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("--luxium", default=os.path.join(os.path.dirname(__file__), "..", "build", "luxium"))
ap.add_argument("--app", default=os.path.join(os.path.dirname(__file__), "..", "..", "Lux-Local", "build", "app"))
ap.add_argument("--display", default=":99")
ap.add_argument("tests", nargs="*")
args = ap.parse_args()
LUXIUM = os.path.realpath(args.luxium)
APP = os.path.realpath(args.app)
assert args.display != ":1", "never the user's desktop"

FAKE_HOST = r'''
import os, signal, socket, sys, time
sock = sys.argv[1]; mode = sys.argv[2]
if mode == "ignoreterm": signal.signal(signal.SIGTERM, signal.SIG_IGN)
s = socket.socket(socket.AF_UNIX); s.bind(sock); s.listen(16)
conns = []
while True:                       # accept, read, never answer: the original bug
    c, _ = s.accept(); conns.append(c)
'''

class Env:
    def __init__(self, name):
        self.root = tempfile.mkdtemp(prefix="hxt-")
        r = self.root
        for d in ("r", "h", "s", "d"): os.makedirs(f"{r}/{d}", exist_ok=True)
        self.rt = f"{r}/r/luxium"
        self.env = dict(os.environ, HOME=f"{r}/h", XDG_RUNTIME_DIR=f"{r}/r", XDG_STATE_HOME=f"{r}/s",
                        DISPLAY=args.display, SDL_AUDIODRIVER="dummy", LUXIUM_BIN=LUXIUM, LUX_NO_DIALOG="1",
                        LUXIUM_HOST_GRACE="60", LUXIUM_HOST_TEST="1")
        self.env.pop("WAYLAND_DISPLAY", None)
        self.procs = []
        self.sock = f"{self.rt}/host.sock"
    def start_app(self, tag="a", **extra):
        e = dict(self.env, XDG_DATA_HOME=f"{self.root}/d{tag}", **extra)
        p = subprocess.Popen([APP], env=e, stdout=open(f"{self.root}/app-{tag}.out", "w"), stderr=subprocess.STDOUT)
        self.procs.append(p)
        return p
    def start_fake(self, mode):
        os.makedirs(self.rt, mode=0o700, exist_ok=True)
        path = f"{self.root}/fake.py"
        open(path, "w").write(FAKE_HOST)
        p = subprocess.Popen([sys.executable, path, self.sock, mode])
        self.procs.append(p)
        for _ in range(100):
            if os.path.exists(self.sock): break
            time.sleep(0.02)
        return p
    def hello(self, timeout=2.0):
        try:
            s = socket.socket(socket.AF_UNIX); s.settimeout(timeout); s.connect(self.sock)
            s.sendall(b'{"cmd":"hello"}\n{"cmd":"ping"}\n')
            buf = b""
            while b"pong" not in buf: buf += s.recv(4096)
            for l in buf.splitlines():
                if b'"hello"' in l: return json.loads(l)
        except OSError:
            return None
    def wait(self, cond, timeout, what):
        end = time.time() + timeout
        while time.time() < end:
            v = cond()
            if v: return v
            time.sleep(0.05)
        raise AssertionError(f"timeout ({timeout}s): {what}\n--- app logs ---\n{self.logs()}")
    def logs(self):
        out = ""
        for f in sorted(glob.glob(f"{self.root}/s/lux-desktop/*.log")):
            out += open(f).read()
        return out
    def log_has(self, text): return text in self.logs()
    def windows(self):
        h = self.hello()
        return h["windows"] if h else -1
    def cleanup(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
                try: p.wait(5)
                except subprocess.TimeoutExpired: p.kill()
        # hosts we spawned through the client are not our children: find them by pidfile
        try:
            pid = int(open(f"{self.rt}/host.pid").read())
            os.kill(pid, signal.SIGKILL)
        except (OSError, ValueError): pass
        shutil.rmtree(self.root, ignore_errors=True)

TESTS = {}
def test(f): TESTS[f.__name__] = f; return f

def recovers(e, mode):
    stuck = e.start_fake(mode)
    t0 = time.time()
    app = e.start_app()
    e.wait(lambda: e.log_has("window opened"), 30, "window opens after recovering")
    dt = time.time() - t0
    assert e.log_has("did not answer hello/ping"), "stuck host not detected"
    if mode == "ignoreterm": assert e.log_has("ignored SIGTERM, sending SIGKILL"), "no SIGKILL escalation"
    assert stuck.wait(5) is not None, "stuck host still alive"
    h = e.hello(); assert h and h["pid"] != stuck.pid and h["windows"] == 1, h
    print(f"    recovered in {dt:.1f}s ({'SIGKILL' if mode == 'ignoreterm' else 'SIGTERM'}), new host pid {h['pid']}")

@test
def stuck_host_sigterm(e): recovers(e, "silent")
@test
def stuck_host_ignores_sigterm(e): recovers(e, "ignoreterm")

@test
def two_apps_at_once(e):
    e.start_fake("silent")
    e.start_app("a"); e.start_app("b")
    e.wait(lambda: e.windows() == 2, 40, "two windows on one host")
    pids = {int(open(f"{e.rt}/host.pid").read())}
    assert e.hello()["pid"] in pids

@test
def version_skew_idle_host_restarted(e):
    first = e.start_app("a")                       # host H1 from build/luxium
    e.wait(lambda: e.windows() == 1, 30, "first window")
    h1 = e.hello()
    first.terminate(); first.wait(15)
    e.wait(lambda: e.windows() == 0, 10, "window gone")
    other = f"{e.root}/luxium-copy"                # a different build: same bytes, other mtime
    shutil.copy(LUXIUM, other); os.utime(other, (1, 1))
    e.env["LUXIUM_BIN"] = other
    e.start_app("b")
    e.wait(lambda: (h := e.hello()) and h["pid"] != h1["pid"] and h["windows"] == 1, 40, "fresh host with new build")
    assert e.log_has("differs from installed") and e.log_has("asked to exit")
    assert e.hello()["build"] != h1["build"]

@test
def version_skew_busy_host_kept(e):
    e.start_app("a")
    e.wait(lambda: e.windows() == 1, 30, "first window")
    h1 = e.hello()
    other = f"{e.root}/luxium-copy"
    shutil.copy(LUXIUM, other); os.utime(other, (1, 1))
    e.env["LUXIUM_BIN"] = other
    e.start_app("b")
    e.wait(lambda: e.windows() == 2, 30, "second window on the old host")
    assert e.hello()["pid"] == h1["pid"] and e.log_has("differs from installed")

@test
def luxium_missing_falls_back(e):
    e.env["LUXIUM_BIN"] = "/nonexistent/luxium"
    app = e.start_app("a", PATH="/usr/bin:/bin")
    e.wait(lambda: e.log_has("using WebKitGTK") or app.poll() is not None, 20, "fallback decision")
    time.sleep(2)
    assert e.log_has("using WebKitGTK"), "no fallback logged"
    assert not os.path.exists(e.sock), "no luxium host may be started"

@test
def host_dies_on_start_falls_back(e):
    e.env["LUXIUM_BIN"] = "/bin/false"
    t0 = time.time()
    e.start_app("a")
    e.wait(lambda: e.log_has("falling back to the WebKitGTK"), 40, "fallback after the host never came up")
    print(f"    fallback after {time.time() - t0:.1f}s")
    assert e.log_has("did not start")

@test
def no_display_error(e):
    env = dict(e.env, XDG_DATA_HOME=f"{e.root}/da"); env.pop("DISPLAY")
    p = subprocess.run([APP], env=env, capture_output=True, timeout=30)
    assert p.returncode != 0 and b"no graphical display" in p.stderr.lower(), (p.returncode, p.stderr[-300:])

@test
def host_sigterm_prompt(e):
    h = subprocess.Popen([LUXIUM, "--host"], env=e.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    e.procs.append(h)
    e.wait(lambda: e.hello(), 10, "host up")
    assert os.path.exists(f"{e.rt}/host.pid")
    t0 = time.time(); h.terminate(); h.wait(10)
    dt = time.time() - t0
    assert dt < 3.5, dt
    assert not os.path.exists(e.sock) and not os.path.exists(f"{e.rt}/host.pid")
    print(f"    idle host exited {dt:.2f}s after SIGTERM")

@test
def host_wedged_main_loop(e):
    h = subprocess.Popen([LUXIUM, "--host"], env=e.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    e.procs.append(h)
    e.wait(lambda: e.hello(), 10, "host up")
    s = socket.socket(socket.AF_UNIX); s.connect(e.sock); s.sendall(b'{"cmd":"_wedge"}\n')   # main thread hangs
    time.sleep(1.5)
    x = e.hello(); assert x and x["beat_ms"] > 1000, x          # reader thread still answers, heartbeat ages
    t0 = time.time(); h.terminate(); h.wait(10)
    dt = time.time() - t0
    assert 2.5 < dt < 4.5, dt                                   # the watchdog, not the (wedged) main loop
    assert not os.path.exists(e.sock)
    print(f"    wedged host died {dt:.2f}s after SIGTERM, ping answered meanwhile (beat_ms={x['beat_ms']})")

@test
def host_wedged_render_loop_replaced(e):
    e.start_app("a")
    e.wait(lambda: e.windows() == 1, 30, "first window")
    h1 = e.hello()
    s = socket.socket(socket.AF_UNIX); s.connect(e.sock); s.sendall(b'{"cmd":"_wedge"}\n')
    time.sleep(11)                                              # heartbeat older than the client's 10 s limit
    e.start_app("b")
    # app a lost its host without a `closed`: it reopens its window in the fresh host, so both apps hold one
    e.wait(lambda: (h := e.hello()) and h["pid"] != h1["pid"] and h["windows"] == 2, 30, "wedged host replaced, both windows back")
    assert e.log_has("render loop stalled")

@test
def quit_command(e):
    h = subprocess.Popen([LUXIUM, "--host"], env=e.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    e.procs.append(h)
    e.wait(lambda: e.hello(), 10, "host up")
    s = socket.socket(socket.AF_UNIX); s.connect(e.sock); s.sendall(b'{"cmd":"quit"}\n')
    assert b"bye" in s.recv(100)
    h.wait(10)

if __name__ == "__main__":
    names = args.tests or list(TESTS)
    failed = []
    for n in names:
        e = Env(n)
        t0 = time.time()
        try:
            TESTS[n](e); print(f"PASS {n} ({time.time() - t0:.1f}s)")
        except Exception as ex:
            failed.append(n); print(f"FAIL {n}: {ex}")
        finally:
            e.cleanup()
    print(f"{len(names) - len(failed)}/{len(names)} passed")
    sys.exit(1 if failed else 0)
