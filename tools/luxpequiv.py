#!/usr/bin/env python3
"""A page compiled to .luxp must render exactly like its HTML.

Packs every css-tests page (plus examples/spa.html after a run of clicks),
renders the HTML and the .luxp, and compares the page area pixel by pixel.
The document, stylesheet and program sections all take part: any
difference is a codec that lost something.

    python3 tools/luxpequiv.py [filter]
"""
import glob, os, subprocess, sys, tempfile
from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# .luxp files are produced by Lux (lux pack); Luxium only opens them.
LUXC = os.environ.get("LUX", os.path.join(ROOT, "..", "Lux", "build", "lux"))
LUX = os.path.join(ROOT, "build", "luxium")
ENV = dict(os.environ, SDL_VIDEODRIVER="dummy")
SPA_CLICKS = ["nav-tasks", "add", "task-2", "del-1", "nav-team", "f2",
              "nav-settings", "th-dark", "acc-violet", "sw-dense"]


def shot(src, out, clicks=()):
    args = [LUX, src]
    for c in clicks:
        args += ["--click", c]
    subprocess.run(args + ["--shot", out], check=True, env=ENV, capture_output=True, timeout=60)
    im = Image.open(out).convert("RGB")
    return im.crop((0, 40, im.width, im.height - 20))   # page area: no URL bar, no status strip


cases = [(p, ()) for p in sorted(glob.glob(os.path.join(ROOT, "css-tests", "**", "*.html"), recursive=True))
         if not p.endswith(".expected.html")]
cases.append((os.path.join(ROOT, "examples", "spa.html"), SPA_CLICKS))
if len(sys.argv) > 1:
    cases = [c for c in cases if sys.argv[1] in c[0]]

bad = 0
with tempfile.TemporaryDirectory() as tmp:
    for path, clicks in cases:
        name = os.path.relpath(path, ROOT)
        luxp = os.path.join(tmp, "t.luxp")
        p = subprocess.run([LUXC, "pack", path, luxp], capture_output=True, text=True, cwd=ROOT)
        if p.returncode != 0:
            print(f"PACK  {name}: {p.stderr.strip()}")
            bad += 1
            continue
        a = shot(path, os.path.join(tmp, "a.ppm"), clicks)
        b = shot(luxp, os.path.join(tmp, "b.ppm"), clicks)
        diff = ImageChops.difference(a, b).getbbox()
        if diff:
            print(f"DIFF  {name}: {diff}")
            bad += 1

print(f"{len(cases) - bad}/{len(cases)} identicas")
sys.exit(1 if bad else 0)
