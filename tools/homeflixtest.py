#!/usr/bin/env python3
"""Drives a live Homeflix in Luxium over CDP: catalog, series page, direct and HEVC-remux playback, audio/subtitle
selects, fullscreen and progress reports.  Read-only on the server (apart from the watch progress the player reports).

    HOMEFLIX_URL=http://host:5001 HOMEFLIX_USER=1 tools/homeflixtest.py

Episode ids are picked from the server's own API: the first series whose first episode is HEVC needing a transcode
(= remux path) and the first H.264 one that has 2+ audio tracks (= direct path, audio switch via the remux).
Needs an X display and build/luxium; the profile picker is used to log in.
"""
import json, os, signal, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(__file__))
from cdp import Page

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BASE = os.environ.get("HOMEFLIX_URL", "http://192.168.1.20:5001").rstrip("/")
USER = os.environ.get("HOMEFLIX_USER", "1")
CDP = int(os.environ.get("LUXIUM_CDP_PORT", 9223))

PIXELS = """(() => { const c = document.querySelector('canvas[data-luxium-surface]'); if (!c) return null;
  const gl = c.getContext('webgl2'); const px = new Uint8Array(4); let n = 0, lit = 0;
  for (let i = 1; i < 8; i++) for (let j = 1; j < 5; j++) { gl.readPixels((c.width * i / 8) | 0, (c.height * j / 5) | 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    n++; if (px[0] + px[1] + px[2] > 60) lit++; }
  return { w: c.width, h: c.height, lit, of: n }; })()"""


def api(path):
    return json.load(urllib.request.urlopen(BASE + path, timeout=30))


def pick():
    remux = direct = None
    for s in api("/api/series"):
        eps = [e for se in api("/api/series/" + s["id"])["seasons"][:1] for e in se["episodes"][:1]]
        if not eps:
            continue
        v, t = api(f"/api/episodes/{eps[0]['id']}/vinfo"), api(f"/api/episodes/{eps[0]['id']}/tracks")
        if not remux and v.get("codec") == "hevc" and v.get("needs_transcode"):
            remux = (s["id"], eps[0]["id"])
        if not direct and v.get("codec") == "h264" and not v.get("needs_transcode") and len(t.get("audio", [])) >= 2:
            direct = (s["id"], eps[0]["id"])
        if remux and direct:
            break
    return remux, direct


def check(res, name, ok, detail=""):
    res.append((name, bool(ok), detail))
    print(("  ok   " if ok else "  FAIL ") + name + (f"  [{detail}]" if detail != "" else ""))


def go(p, path):
    p.eval(f"navigate({json.dumps(path)})")


def key(p, k, code):
    for t in ("keyDown", "keyUp"):
        p.call("Input.dispatchKeyEvent", type=t, key=k, code=code, text=k if t == "keyDown" else "",
               windowsVirtualKeyCode=ord(k.upper()))


def play_checks(res, p, label, src_part):
    p.wait_for("document.getElementById('player') && document.getElementById('player').readyState >= 3", 40)
    v = "document.getElementById('player')"
    check(res, f"{label}: source", src_part in p.eval(f"{v}.currentSrc || {v}.src"), p.eval(f"{v}.currentSrc || {v}.src"))
    if p.eval(f"{v}.paused"):
        p.eval(f"{v}.play()")
    t0 = p.eval(f"{v}.currentTime")
    time.sleep(3)
    t1 = p.eval(f"{v}.currentTime")
    check(res, f"{label}: plays (currentTime advances)", 2.0 < t1 - t0 < 4.5, f"{t1 - t0:.2f}s in 3s")
    px = p.eval(PIXELS)
    check(res, f"{label}: canvas not black", px and px["lit"] >= px["of"] // 4, json.dumps(px))
    st = p.eval("window.__luxiumMediaStats()")
    check(res, f"{label}: no excess drops", st and st[-1]["frames"] > 20, json.dumps(st[-1] if st else None))


def main():
    remux, direct = pick()
    print("remux episode:", remux, " direct episode:", direct)
    res = []
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", LUXIUM_CDP_PORT=str(CDP))
    proc = subprocess.Popen([os.path.join(ROOT, "build/luxium"), BASE + "/"], env=env, cwd=os.path.join(ROOT, "build"),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        p = Page(CDP, wait=40)
        p.wait_for("document.readyState === 'complete'", 30)
        # profile picker
        p.wait_for(f"document.querySelector('.user-card[data-user-id=\"{USER}\"]')", 20)
        p.eval(f"document.querySelector('.user-card[data-user-id=\"{USER}\"]').click()")
        p.eval("window.__beats = []; const f = window.fetch; window.fetch = (u, o) => { if (String(u).includes('heartbeat')) window.__beats.push(String(u)); return f(u, o); }")
        # catalog
        p.wait_for("document.querySelectorAll('.poster-grid a').length > 3", 30)
        time.sleep(3)
        r = p.eval("(() => { const im = [...document.querySelectorAll('.poster-grid img')]; return [im.length, im.filter(i => i.complete && i.naturalWidth > 0).length]; })()")
        check(res, "catalog renders posters", r[0] > 3 and r[1] >= r[0] * 0.9, f"{r[1]}/{r[0]} images loaded")
        # series page
        sid = remux[0]
        go(p, f"/serie/{sid}")
        p.wait_for("document.querySelectorAll('.episode-row').length > 0", 20)
        time.sleep(2)
        r = p.eval("""(async () => { const bg = document.querySelector('.banner').style.backgroundImage.slice(5, -2);
          const ok = await new Promise(res => { const i = new Image(); i.onload = () => res(i.naturalWidth); i.onerror = () => res(0); i.src = bg; });
          const im = [...document.querySelectorAll('img')]; return [ok, im.length, im.filter(i => i.complete && i.naturalWidth > 0).length]; })()""")
        check(res, "series page: backdrop and images load", r[0] > 0 and r[2] >= r[1] * 0.9, f"backdrop {r[0]}px, {r[2]}/{r[1]} img")

        # direct path
        sid, eid = direct
        go(p, f"/serie/{sid}/ver/{eid}")
        play_checks(res, p, "direct", "/api/stream/")
        p.eval("document.getElementById('player').currentTime = 20")
        p.wait_for("!document.getElementById('player').seeking", 15)
        # subtitles: pick the first track, find a cue in the first minutes, check the overlay
        p.eval("(() => { const s = document.getElementById('subs-select'); s.value = '0'; s.dispatchEvent(new Event('change')); })()")
        p.wait_for("document.querySelector('track') && document.getElementById('player').textTracks.length && document.getElementById('player').textTracks[0].cues && document.getElementById('player').textTracks[0].cues.length > 0", 20)
        cue = p.eval("(() => { const c = document.getElementById('player').textTracks[0].cues[0]; return [c.startTime, c.endTime, c.text]; })()")
        check(res, "direct: subtitle cues loaded", cue is not None, json.dumps(cue))
        p.eval(f"document.getElementById('player').currentTime = {cue[0] + 0.3}")
        p.eval("document.getElementById('player').play()")
        p.wait_for("document.getElementById('subs-overlay').textContent.trim().length > 0", 10)
        check(res, "direct: subtitles painted in the overlay (activeCues + getCueAsHTML)",
              p.eval("document.getElementById('subs-overlay').innerHTML"), p.eval("document.getElementById('subs-overlay').textContent"))
        # audio switch -> remux HLS
        p.eval("(() => { const s = document.getElementById('audio-select'); s.value = '1'; s.dispatchEvent(new Event('change')); })()")
        time.sleep(1.5)
        p.wait_for("document.getElementById('player').readyState >= 3 && (document.getElementById('player').currentSrc || '').includes('/api/rm/')", 60)
        t0 = p.eval("document.getElementById('player').currentTime")
        time.sleep(3)
        t1 = p.eval("document.getElementById('player').currentTime")
        check(res, "audio select: switches to the remux HLS and keeps playing", t1 - t0 > 2, f"{t1 - t0:.2f}s, {p.eval('document.getElementById(\"player\").currentSrc')}")
        # fullscreen (key f on the wrapper: needs a real user gesture, hence Input.dispatchKeyEvent)
        p.eval("document.activeElement.blur && document.activeElement.blur()")
        key(p, "f", "KeyF")
        try:
            p.wait_for("document.fullscreenElement && document.fullscreenElement.id === 'video-wrap'", 5)
            check(res, "fullscreen: f enters on #video-wrap", True)
            key(p, "f", "KeyF")
            p.wait_for("!document.fullscreenElement", 5)
            check(res, "fullscreen: f leaves", True)
        except TimeoutError:
            check(res, "fullscreen toggles", False, str(p.eval("String(document.fullscreenElement)")))
        # progress
        p.eval("document.getElementById('player').pause()")
        time.sleep(1)
        beats = p.eval("window.__beats")
        check(res, "progress reported (heartbeat)", any("heartbeat" in b for b in beats), (beats or [""])[-1])

        # HEVC remux path
        sid, eid = remux
        go(p, f"/serie/{sid}/ver/{eid}")
        play_checks(res, p, "remux", "/api/rm/")
        check(res, "remux: hint shown", p.eval("(document.querySelector('.track-hint')||{}).textContent") is not None)
        p.eval("document.getElementById('player').currentTime = 60")
        p.wait_for("!document.getElementById('player').seeking", 30)
        check(res, "remux: seek", abs(p.eval("document.getElementById('player').currentTime") - 60) < 8, p.eval("document.getElementById('player').currentTime"))
    except Exception as e:
        import traceback; traceback.print_exc()
        check(res, "scenario", False, repr(e))
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
    bad = [r for r in res if not r[1]]
    print("FAILED: %d" % len(bad) if bad else "all checks passed")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
