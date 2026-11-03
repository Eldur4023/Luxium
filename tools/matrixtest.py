#!/usr/bin/env python3
"""Format matrix: plays every file of examples/media/fixtures/matrix DIRECTLY in Luxium (no remux, no
transcode) and checks canPlayType, navigator.luxiumMedia, picture, audio, audio-track switching, subtitles.

    tools/media_matrix_fixtures.sh         # once
    DISPLAY=:99 tools/matrixtest.py [fixture-substring ...]

Honours LUXIUM_CDP_PORT (default 9261) and MATRIX_PORT (range server, default 8782).
Starts its own range server and Luxium (SDL dummy audio) and closes them.
"""
import json, os, signal, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from cdp import Page

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PORT, CDP = int(os.environ.get("MATRIX_PORT", 8782)), int(os.environ.get("LUXIUM_CDP_PORT", 9261))
# file, container mime, video codec, audio codec(s) (ffprobe names), audio tracks, text-subtitle tracks, interlaced
M = [
    ("hevc10_ac3_aac_subs.mkv", "video/x-matroska", "hevc", ["ac3", "aac"], 2, 2, False),
    ("h264_dts.mkv", "video/x-matroska", "h264", ["dts"], 1, 0, False),
    ("h264_flac.mkv", "video/x-matroska", "h264", ["flac"], 1, 0, False),
    ("h264_truehd.mkv", "video/x-matroska", "h264", ["truehd"], 1, 0, False),
    ("h264_444_aac.mkv", "video/x-matroska", "h264", ["aac"], 1, 0, False),
    ("xvid_mp3.avi", "video/x-msvideo", "mpeg4", ["mp3"], 1, 0, False),
    ("prores_pcm.mov", "video/quicktime", "prores", ["pcm_s16le"], 1, 0, False),
    ("wmv2_wma.wmv", "video/x-ms-wmv", "wmv2", ["wmav2"], 1, 0, False),
    ("mpeg2_interlaced_mp2.ts", "video/mp2t", "mpeg2video", ["mp2"], 1, 0, True),
    ("flv1_mp3.flv", "video/x-flv", "flv1", ["mp3"], 1, 0, False),
    ("h264_aac_flv.flv", "video/x-flv", "h264", ["aac"], 1, 0, False),
    ("av1_opus.webm", "video/webm", "av1", ["opus"], 1, 0, False),
    ("vp9_vorbis.webm", "video/webm", "vp9", ["vorbis"], 1, 0, False),
    ("h264_hi10p_eac3.mp4", "video/mp4", "h264", ["eac3"], 1, 0, False),
    ("mpeg4_aac.mp4", "video/mp4", "mpeg4", ["aac"], 1, 0, False),
]
PIXELS = """(() => { const c = document.querySelector('canvas[data-luxium-surface]'); if (!c) return null;
  const gl = c.getContext('webgl2'); const px = new Uint8Array(4); let n = 0, lit = 0;
  for (let i = 1; i < 8; i++) for (let j = 1; j < 5; j++) { gl.readPixels((c.width * i / 8) | 0, (c.height * j / 5) | 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    n++; if (px[0] + px[1] + px[2] > 60) lit++; }
  return { w: c.width, h: c.height, lit, of: n }; })()"""
STATS = "(window.__luxiumMediaStats()[0] || {})"


def ok(res, name, cond, detail=""):
    res.append((name, bool(cond), detail))
    print(("  ok   " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail != "" else ""))


def one(p, row):
    name, mime, vc, acs, na, ns, interlaced = row
    res = []
    cp = p.eval(f"document.createElement('video').canPlayType({json.dumps(mime)})")
    ok(res, f"canPlayType({mime})", cp in ("maybe", "probably"), cp)
    cp2 = p.eval(f"document.createElement('video').canPlayType({json.dumps(mime + '; codecs=\"' + vc + ',' + acs[0] + '\"')})")
    ok(res, f"canPlayType with codecs {vc},{acs[0]}", cp2 == "probably", cp2)
    lm = p.eval(f"(() => {{ const m = navigator.luxiumMedia; return m && [m.canDecodeVideo({json.dumps(vc)}), m.canDecodeAudio({json.dumps(acs[0])})] }})()")
    ok(res, "navigator.luxiumMedia says decodable", lm == [True, True], lm)
    p.wait_for("document.getElementById('v').readyState >= 3 || window.__ev.includes('error')", 25)
    ok(res, "no error, canplay", "error" not in p.eval("window.__ev") and p.eval("document.getElementById('v').readyState >= 3"), p.eval("window.__ev"))
    ok(res, "audioTracks", p.eval("document.getElementById('v').audioTracks && document.getElementById('v').audioTracks.length") == na,
       p.eval("document.getElementById('v').audioTracks && [...document.getElementById('v').audioTracks].map(t => t.id + ':' + t.language + ':' + t.enabled)"))
    p.eval("document.getElementById('v').play()")
    t0 = p.eval("document.getElementById('v').currentTime")
    time.sleep(2.5)
    t1 = p.eval("document.getElementById('v').currentTime")
    ok(res, "currentTime advances", 1.8 < t1 - t0 < 3.6, f"{t1 - t0:.2f}s in 2.5s")
    px = p.eval(PIXELS)
    if px is not None:   # no surface = no WebGL2 (Xvfb without GL): frames are still decoded and counted below
        ok(res, "canvas not black", px["lit"] >= px["of"] // 3, json.dumps(px))
    s = p.eval(STATS)["engine"]
    ok(res, "frames dropped <= 3", s["dropped"] <= 3, f"{s['presented']} presented, {s['dropped']} dropped, hw={s['hw']}, {s['vcodec']} {s['pixFmt']}")
    ok(res, f"audio produced ({s['acodec']}, {s['audioChannels']} ch)", s["audioFrames"] > 20000 and s["audioPeak"] > 0.02, f"{s['audioFrames']} frames, peak {s['audioPeak']}")
    if interlaced:
        ok(res, "interlaced detected + deinterlaced", s["deinterlace"])
    if na > 1:
        sw = p.eval("(() => { const v = document.getElementById('v'); const a = v.audioTracks; window.__chg = 0; a.addEventListener('change', () => window.__chg++);"
                    " a[1].enabled = true; return [a[0].enabled, a[1].enabled] })()")
        ok(res, "audioTracks[1].enabled = true switches", sw == [False, True], sw)
        time.sleep(0.4)
        p.wait_for("!document.getElementById('v').seeking", 8)
        before = p.eval(STATS)["engine"]
        t2 = p.eval("document.getElementById('v').currentTime")
        time.sleep(1.5)
        after = p.eval(STATS)["engine"]
        t3 = p.eval("document.getElementById('v').currentTime")
        ok(res, "playback continues at the same position after the switch", t2 >= t1 - 0.6 and 1.0 < t3 - t2 < 2.2, f"{t1:.2f} -> {t2:.2f} -> {t3:.2f}")
        ok(res, f"new audio codec plays ({after['acodec']})", after["acodec"] == acs[1] and after["audioFrames"] - before["audioFrames"] > 40000 and after["audioPeak"] > 0.02,
           f"{after['audioFrames'] - before['audioFrames']} frames")
        ok(res, "change event fired once", p.eval("window.__chg") == 1, p.eval("window.__chg"))
        ok(res, "same URL (no reopen)", "loadedmetadata" in p.eval("window.__ev") and p.eval("window.__ev").count("loadedmetadata") == 1)
        # back to the first
        p.eval("document.getElementById('v').audioTracks[0].enabled = true")
        p.wait_for("document.getElementById('v').audioTracks[0].enabled && !document.getElementById('v').seeking", 8)
        time.sleep(1.2)
        ok(res, f"switch back ({p.eval(STATS)['engine']['acodec']})", p.eval(STATS)["engine"]["acodec"] == acs[0])
    if ns:
        n = p.eval("document.getElementById('v').textTracks.length")
        ok(res, "embedded text subtitle tracks", n == ns, n)
        p.eval("(() => { const v = document.getElementById('v'); v.pause(); v.currentTime = 0; for (const t of v.textTracks) t.mode = 'hidden'; v.play() })()")
        got = {}
        end = time.time() + 6
        while time.time() < end:
            r = p.eval("[...document.getElementById('v').textTracks].map(t => [t.label, t.language, [...(t.activeCues || [])].map(c => c.text)])")
            for lab, lang, cues in r:
                if cues: got.setdefault(lab + "/" + lang, set()).update(cues)
            time.sleep(0.15)
        ok(res, "cues appear on every embedded track", len(got) == ns, json.dumps({k: sorted(v) for k, v in got.items()}, ensure_ascii=False))
        if got:
            txt = " ".join(" ".join(v) for v in got.values())
            ok(res, "ASS styling -> <i>, \\N -> newline", "<i>ASS</i>" in txt and "Second row" in txt, txt[:120])
    p.eval("document.getElementById('v').pause()")
    return res


def launch():
    base = "/tmp/lxd"
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", LUXIUM_CDP_PORT=str(CDP), LUXIUM_PROFILE=base + "/p", HOME=base + "/h", XDG_RUNTIME_DIR=base + "/r")
    return subprocess.Popen([os.path.join(ROOT, "build/luxium"), f"http://127.0.0.1:{PORT}/matrix.html?f=fixtures/matrix/{M[0][0]}"],
                            env=env, cwd=os.path.join(ROOT, "build"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def main():
    want = sys.argv[1:]
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools/rangeserver.py"), str(PORT), os.path.join(ROOT, "examples/media")], start_new_session=True)
    time.sleep(0.5)
    proc = launch()
    failed = 0
    try:
        p = Page(CDP, wait=40)
        for row in M:
            if want and not any(w in row[0] for w in want):
                continue
            print(row[0])
            try:
                p.call("Page.navigate", url=f"http://127.0.0.1:{PORT}/matrix.html?f=fixtures/matrix/{row[0]}")
                time.sleep(0.8)
                p.wait_for("document.readyState === 'complete' && !!document.getElementById('v')", 20)
                res = one(p, row)
            except Exception as e:
                res = [("scenario", False, repr(e))]
                print("  FAIL scenario:", repr(e))
            failed += sum(1 for r in res if not r[1])
    finally:
        for pr in (proc, srv):
            try:
                os.killpg(pr.pid, signal.SIGTERM)
                pr.wait(10)
            except Exception:
                try: os.killpg(pr.pid, signal.SIGKILL)
                except Exception: pass
    print("FAILED checks:", failed)
    sys.exit(1 if failed else 0)


main()
