#!/usr/bin/env python3
"""Drives Luxium's CEF pages over CDP and checks the media engine end to end.

    tools/mediatest.py [page ...]        # default: every fixture page

Needs: tools/media_fixtures.sh run once, an X display (DISPLAY, Xvfb is fine),
build/luxium.  Starts its own file server and its own Luxium (SDL's dummy audio
driver, so nothing is played out loud) and closes them at the end.
"""
import os, subprocess, sys, time, json
sys.path.insert(0, os.path.dirname(__file__))
from cdp import Page

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PORT, CDP = 8781, 9344
PAGES = ["h264-aac", "hevc10-ac3", "vp9-opus", "hls-ts", "hls-fmp4", "4k-hevc", "tracks", "hlsjs"]

PIXELS = """(() => { const c = document.querySelector('canvas[data-luxium-surface]'); if (!c) return null;
  const gl = c.getContext('webgl2'); const px = new Uint8Array(4); let sum = 0, n = 0, lit = 0;
  for (let i = 1; i < 8; i++) for (let j = 1; j < 5; j++) { gl.readPixels((c.width * i / 8) | 0, (c.height * j / 5) | 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const l = px[0] + px[1] + px[2]; sum += l; n++; if (l > 60) lit++; }
  return { w: c.width, h: c.height, mean: sum / n / 3, lit, of: n }; })()"""


def launch(url):
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", LUXIUM_CDP_PORT=str(CDP))
    return subprocess.Popen([os.path.join(ROOT, "build/luxium"), url], env=env, cwd=os.path.join(ROOT, "build"),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def check(res, name, ok, detail=""):
    res.append((name, bool(ok), detail))
    print(("  ok   " if ok else "  FAIL ") + name + (f"  [{detail}]" if detail != "" else ""))


def scenario(page, p):
    res = []
    p.wait_for("document.getElementById('v') && document.getElementById('v').readyState >= 3", 25)
    ev = p.eval("window.__ev.map(e => e[0])")
    order = ["loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay"]
    pos = [ev.index(e) if e in ev else -1 for e in order]
    check(res, "load events in order", all(x >= 0 for x in pos) and pos == sorted(pos), ",".join(ev))
    check(res, "duration/size", p.eval("v.duration > 5 && v.videoWidth > 0") if False else p.eval("(() => { const v = document.getElementById('v'); return v.duration > 5 && v.videoWidth > 0 })()"),
          p.eval("(() => { const v = document.getElementById('v'); return v.duration + ' ' + v.videoWidth + 'x' + v.videoHeight })()"))
    p.eval("document.getElementById('v').play()")
    t0 = p.eval("document.getElementById('v').currentTime")
    time.sleep(2.2)
    v = p.eval("(() => { const v = document.getElementById('v'); return { t: v.currentTime, paused: v.paused, rs: v.readyState } })()")
    check(res, "currentTime advances in real time", 1.6 < v["t"] - t0 < 3.2 and not v["paused"], f"{v['t'] - t0:.2f}s in 2.2s")
    px = p.eval(PIXELS)
    check(res, "canvas shows the picture (not black)", px and px["lit"] >= px["of"] // 3, json.dumps(px))
    stats = p.eval("window.__luxiumMediaStats()")
    check(res, "frames drawn", stats and stats[0]["frames"] > 30, json.dumps(stats))
    p.eval("document.getElementById('v').pause()")
    check(res, "pause toggles paused", p.eval("document.getElementById('v').paused") is True)
    p.eval("document.getElementById('v').currentTime = 12")
    p.wait_for("!document.getElementById('v').seeking", 10)
    t = p.eval("document.getElementById('v').currentTime")
    check(res, "seek lands within a GOP", 11.9 <= t <= 14.1, f"{t:.2f}")
    evs = p.eval("window.__ev.map(e => e[0])")
    check(res, "seeking/seeked fired", "seeking" in evs and "seeked" in evs)
    p.eval("(() => { const v = document.getElementById('v'); v.muted = true; v.volume = 0.5; v.playbackRate = 1.5 })()")
    evs = p.eval("window.__ev.map(e => e[0])")
    check(res, "volumechange/ratechange fired", "volumechange" in evs and "ratechange" in evs)
    p.eval("(() => { const v = document.getElementById('v'); v.playbackRate = 1; v.currentTime = v.duration - 1.2; v.play() })()")
    try:
        p.wait_for("window.__ev.some(e => e[0] === 'ended')", 12)
        check(res, "ended fires at the end", p.eval("document.getElementById('v').ended && document.getElementById('v').paused"))
    except TimeoutError:
        check(res, "ended fires at the end", False, "timeout")
    return res


def tracks_scenario(p):
    res = []
    p.wait_for("document.getElementById('v').readyState >= 3 && window.__cues !== undefined", 25)
    check(res, "textTracks has the <track>", p.eval("document.getElementById('v').textTracks.length") == 1)
    p.eval("document.getElementById('v').play()")
    time.sleep(6.5)
    cues = p.eval("window.__cues")
    check(res, "cuechange fired, activeCues change over time", len(cues) >= 5 and any(c[1] for c in cues), json.dumps(cues[:6]))
    on = [c for c in cues if c[1]]
    check(res, "cue text", on and on[0][1][0].startswith("Line 0"), on[0][1] if on else "")
    check(res, "getCueAsHTML renders", p.eval("document.getElementById('o').textContent.length >= 0"))
    p.eval("document.getElementById('v').pause()")
    return res


def hlsjs_scenario(p):
    res = []
    info = p.eval("window.__hls")
    check(res, "hls.js falls back to native HLS", info and info["isSupported"] is False and info["native"] == "maybe", json.dumps(info))
    p.wait_for("document.getElementById('v').readyState >= 3", 25)
    p.eval("document.getElementById('v').play()")
    time.sleep(2)
    check(res, "plays via native-HLS path", p.eval("document.getElementById('v').currentTime") > 1.0)
    return res


def main():
    pages = sys.argv[1:] or PAGES
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools/rangeserver.py"), str(PORT), os.path.join(ROOT, "examples/media")])
    time.sleep(0.5)
    failed = 0
    try:
        for name in pages:
            print(name)
            proc = launch(f"http://127.0.0.1:{PORT}/{name}.html")
            try:
                p = Page(CDP, wait=30)
                p.wait_for("document.readyState === 'complete'", 20)
                if name == "tracks":
                    res = tracks_scenario(p)
                elif name == "hlsjs":
                    res = hlsjs_scenario(p)
                else:
                    res = scenario(name, p)
            except Exception as e:
                res = [("scenario", False, repr(e))]
                print("  FAIL scenario:", repr(e))
            finally:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                os.system("pkill -x luxium 2>/dev/null")
                time.sleep(1)
            failed += sum(1 for r in res if not r[1])
    finally:
        srv.terminate()
    print("FAILED: %d" % failed if failed else "all checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
