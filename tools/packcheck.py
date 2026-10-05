#!/usr/bin/env python3
"""Round-trip check for .luxp (pages compiled to document + rules + bytecode): pack examples/regress/pack, open the bundle
from a directory where the sources do NOT exist, and check pixels that only
come out right if every asset was read from the bundle:
  - h1 colour from css/extra.css (@import inside css/main.css)
  - .hero background from url(img/tile.png)
  - <img src=img/red.png>
  - the script ran (title text changed -> h1 wider)
  - clicking the <a href=otra.html> navigates inside the bundle

    python3 tools/packcheck.py      (needs build/luxium)
"""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import luxpfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# .luxp files are produced by Lux (lux pack); Luxium only opens them.
LUXC = os.environ.get("LUX", os.path.join(ROOT, "..", "Lux", "build", "lux"))
LUX = os.path.join(ROOT, "build", "luxium")
ENV = dict(os.environ, SDL_VIDEODRIVER="dummy")


def shot(luxp, out, *clicks):
    args = [LUX, luxp]
    for c in clicks:
        args += ["--click", c]
    subprocess.run(args + ["--shot", out], check=True, env=ENV, capture_output=True)
    return Image.open(out).convert("RGB")


def near(px, rgb, tol=12):
    return all(abs(a - b) <= tol for a, b in zip(px, rgb))


with tempfile.TemporaryDirectory() as tmp:
    luxp = os.path.join(tmp, "site.luxp")
    subprocess.run([LUXC, "pack", os.path.join(ROOT, "examples/regress/pack/index.html"), luxp],
                   check=True, capture_output=True, cwd=ROOT)
    img = shot(luxp, os.path.join(tmp, "a.ppm"))
    w, h = img.size
    px = img.load()
    purple = sum(near(px[x, y], (102, 51, 153), 40) for x in range(0, 400) for y in range(40, 100))
    title_right = max((x for x in range(w) for y in range(50, 90, 4) if near(px[x, y], (102, 51, 153), 60)), default=0)
    checks = {
        "@import css (h1 morado)": purple > 50,
        "script ejecutado (titulo largo)": title_right > 200,
        "background url()": near(px[300, 170], (60, 160, 230)),
        "<img>": near(px[80, 258], (230, 80, 60)),
    }
    payload = luxpfile.plain_bytes(luxpfile.read(luxp))
    checks["sin HTML/CSS/LuxScript en texto"] = all(
        t not in payload for t in (b"<html", b"<script", b"@import", b"{ color", b"set_text("))
    checks["--inspect verifica el bytecode"] = subprocess.run(
        [LUX, "--inspect", luxp], capture_output=True).returncode == 0
    nav = shot(luxp, os.path.join(tmp, "b.ppm"), "go").load()
    checks["enlace dentro del .luxp"] = not near(nav[80, 258], (230, 80, 60))

bad = [k for k, ok in checks.items() if not ok]
for k, ok in checks.items():
    print(("ok   " if ok else "FAIL ") + k)
sys.exit(1 if bad else 0)
