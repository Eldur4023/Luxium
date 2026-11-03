#!/usr/bin/env python3
"""SPA API tests: starts tools/spa_server.py (own process group, random port),
runs build/luxium_headless against examples/spa/*.html with scripted steps and
checks the console output.  No window, no CEF.

  tools/spatest.py [HEADLESS_BINARY]
"""
import os, signal, subprocess, sys, tempfile, time, socket

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
HEADLESS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "luxium_headless")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


PORT = free_port()
BASE = f"http://127.0.0.1:{PORT}"
PROFILE = tempfile.mkdtemp(prefix="luxium-spatest-")
failed = 0
passed = 0


def run(target, steps=(), env=None, timeout=30):
    e = dict(os.environ, LUXIUM_PROFILE_DIR=PROFILE, **(env or {}))
    p = subprocess.run([HEADLESS, target, *steps], cwd=ROOT, capture_output=True, text=True, env=e,
                       timeout=timeout, start_new_session=True)
    return p.stdout + p.stderr


def check(name, out, *needles, absent=()):
    global failed, passed
    missing = [n for n in needles if n not in out]
    present = [n for n in absent if n in out]
    if missing or present:
        failed += 1
        print(f"  FAIL {name}")
        for n in missing:
            print(f"       missing: {n!r}")
        for n in present:
            print(f"       unexpected: {n!r}")
        print("       --- output ---\n" + "\n".join("       " + l for l in out.splitlines()[-40:]))
    else:
        passed += 1
        print(f"  ok   {name}")


def in_order(out, *needles):
    pos = 0
    for n in needles:
        i = out.find(n, pos)
        if i < 0:
            return False
        pos = i + len(n)
    return True


srv = subprocess.Popen([sys.executable, os.path.join(HERE, "spa_server.py"), str(PORT)], start_new_session=True)
try:
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), 0.2).close()
            break
        except OSError:
            time.sleep(0.1)

    print("== http / json (examples/spa/http.html)")
    o = run(f"{BASE}/http.html", ["until:top level done:8000", "click:slow1", "click:slow2", "wait:900"])
    check("GET json, query string, relative URL", o, "get: 200 ok=true hello=world n1=2 a=1")
    check("POST json body sets Content-Type", o, "post json: 201 ctype=application/json name=Zoe")
    check("POST string body with custom header", o, "post text: plain text body")
    check("4xx is a Response, not an error", o, "404: ok=false status=404 err=nothing here")
    check("header() + text() on non-json", o, "header: yes text=<html>not json</html>")
    check("json() raises on a non-json body", o, "json error caught: json(): the response body is not valid JSON")
    check("dropped connection is catchable", o, "network error caught")
    check("cross-origin is refused", o, "cross-origin: http: cross-origin request blocked")
    check("request headers", o, "headers: 42")
    check("put/patch/delete", o, "verbs: PUT PATCH 204")
    check("json.parse / stringify", o, 'json: 2 x {"k":[1,"two"]}', "json.parse error caught")
    check("concurrent coroutines resume in completion order", o, absent=["error]"])
    check("slow handlers", o, "slow done two")
    if not in_order(o, "slow done two", "slow done one"):
        check("slow order", "", "two before one")
    out_file = run("examples/spa/http.html")
    check("file:// pages get no network", out_file, "has no network origin")

    print("== DOM, events, templates (examples/spa/dom.html)")
    o = run("examples/spa/dom.html", ["click:card2", "click:logo", "click:box", "type:q=hello", "key:a@q",
                                      "key:Enter@q", "click:sub", "dblclick:box", "click:dok"])
    check("template.render over a string", o, "cards: 2 first=One second=T<wo>", "p: 2 items html-has-escape=true")
    check("set_html drops <script>", o, "script dropped: 0", absent=["SHOULD NOT RUN"])
    check("classes / style / data / closest", o, "class: a c has c=true has b=false", "style: width: 10px; width=10px",
          "data: 7 7", "closest: app parent=main", "query null: true")
    check("create / prepend / insert_before / replace_with / remove", o, "order: cloned,made count=5",
          "after remove: connected=true first=cloned count=4")
    check("capture before target, delegation with closest + prevent", o, "capture main_el", "doc click link /serie/2 id=2")
    check("stop() ends propagation", o, "stopper", absent=["doc click on non-link a"])
    check("click on non-link bubbles to document", o, "doc click on non-link div")
    check("input / change events carry the value", o, "input value=hello via-element=hello", "change value=hello")
    check("keydown reaches document, prevent() on Enter blocks submit", o, "key keydown a ctrl=false target=q",
          "key keydown Enter ctrl=false target=q")
    check("submit via button reads form controls", o, "submit q=hello sel=a ck=false")
    check("<dialog> show_modal + method=dialog close value", o, "dialog closed with ok")
    check("select value / selected_text", o, "select: a text=Aaa")

    print("== storage, history, timers, ws (examples/spa/state.html)")
    url = f"{BASE}/state.html?q=7#t=tok123"
    o = run(url, ["until:on_close:4000", "wait:150"])
    check("storage set/get/remove/keys", o, "storage visits=1 user=Ana keys=2", "removed: true")
    check("page.location: path, query, hash, fragment (Lux-Local token)", o, "loc path=/state.html q=7 hash=t=tok123 frag-t=tok123")
    check("history push/replace/back/forward + on_navigate", o, "pushed: /serie/9 x=1", "replaced: /serie/10",
          "after back: /serie/10", "after forward: /other", "popstate -> /other", "push cross-origin refused")
    check("every/after/cancel", o, "tick 1", "tick 2", "tick 3", "after fired", absent=["tick 4"])
    check("await page.frame()", o, "two frames later")
    check("page.title()", o, "title: Changed title / doc Changed title")
    check("ws: wait_open, recv, send, Origin header", o, "ws open=true", "greeting: hello origin=" + BASE, "recv: echo:ping")
    check("ws: on_message / on_close handlers + recv returns null when closed", o, "on_message echo:handler-style",
          "recv after bye: true open=false", "on_close code=4001")
    check("math / url / time", o, "math: 2 3 3.1 9 4", "url: a%20b%26c%2F%C3%A9 a b two words", "time ok: true")
    o = run(url, ["wait:100"])
    check("storage persists across runs of the same origin", o, "storage visits=2")

    print("== upload (examples/spa/upload.html)")
    o = run(f"{BASE}/upload.html", ["wait:500"])
    check("File from input, Form + multipart", o, "files: 1 name=sample.bin size=25",
          "manual: 2 parts; title=Manual; image file=sample.bin size=25")
    check("Form.progress after an upload", o, "progress total>0: true sent==total: true")
    check("form_data(form) collects controls and files", o, "from form: 2 parts; From form; avatar/sample.bin")

    print("== media (examples/spa/media.html, scripted player)")
    o = run("examples/spa/media.html", ["until:run done:8000"], env={"LUXIUM_FAKE_MEDIA": "1"})
    check("media events: loadedmetadata/play/pause/seeked/ended/cuechange", o, "event loadedmetadata dur=10",
          "event play paused=false", "event pause", "event seeked t=1", "event cuechange cue=[cue text]", "event ended")
    check("tracks + select_audio/select_text", o, "tracks: audio=2 text=2 current=track1", "audio now track2 current=true", "text off false")
    check("volume / muted", o, "vol 0.5 muted=true")
    check("timeupdate fires", o, "timeupdates>0: true")

    print("== media, real file (examples/spa/media-real.html, window binary on Xvfb :99, dummy audio)")
    win = os.path.join(ROOT, "build", "luxium")
    if os.path.exists(win) and os.path.exists("/tmp/.X11-unix/X99"):
        e = dict(os.environ, DISPLAY=":99", SDL_AUDIODRIVER="dummy", LUXIUM_PROFILE_DIR=PROFILE)
        p = subprocess.run([win, "examples/spa/media-real.html", "--wait", "8000", "--shot", os.path.join(PROFILE, "m.ppm")],
                           cwd=ROOT, capture_output=True, text=True, env=e, timeout=60, start_new_session=True)
        check("video.duration/play/pause/seek/current_time/paused on h264_aac.mp4", p.stdout + p.stderr,
              "real dur>0: true", "real paused0=true", "real paused1=false t>0.3: true", "real paused2=true", "real seek ok: true")
    else:
        print("  skip  needs build/luxium and Xvfb :99")

    print("== compat: M4 demo pages")
    o = run("examples/counter.html", ["mas", "mas"])
    check("counter.html still works (click(fn) + page.clicked)", o, "contador: 2")
finally:
    try:
        os.killpg(srv.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    srv.wait()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
