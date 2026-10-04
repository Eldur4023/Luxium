#!/usr/bin/env python3
# cssgolden.py — genera los goldens de css-tests desde el ORACULO:
# cada pagina se renderiza con Chromium (CEF off-screen, --cefshot) y el
# area de pagina del framebuffer se guarda como css-tests/golden/<id>.png.
#
# Requiere sesion X11 (DISPLAY=:0); la ventana es oculta.
# Uso:
#   python3 tools/cssgolden.py               # todo (salta goldens existentes)
#   python3 tools/cssgolden.py --force box/box-sizing
#   python3 tools/cssgolden.py box flex      # directorios/ficheros concretos
import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
TESTS = REPO / "css-tests"
GOLDEN = TESTS / "golden"
PPM = Path("/tmp/opencode/cssgolden.ppm")
URL_BAR_H = 38
STATUS_H = 20


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true", help="regenerar existentes")
    ap.add_argument("targets", nargs="*")
    args = ap.parse_args()

    rels = sorted(str(p.relative_to(TESTS))
                  for p in TESTS.rglob("*.html") if p.is_file())
    if args.targets:
        wanted = []
        for t in args.targets:
            base = t[:-len(".html")] if t.endswith(".html") else t
            cand = [r for r in rels if r == base + ".html" or r.startswith(base.rstrip("/") + "/")]
            wanted += cand
        rels = wanted

    GOLDEN.mkdir(exist_ok=True)
    ok = skip = bad = 0
    for rel in rels:
        out = GOLDEN / (rel[:-len(".html")] + ".png")
        if out.exists() and not args.force:
            skip += 1
            continue
        try:
            subprocess.run(
                [str(REPO / "build" / "luxium"), str(TESTS / rel),
                 "--cefshot", str(PPM)],
                cwd=REPO, check=True, timeout=60,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            img = Image.open(PPM).convert("RGB")
            w, h = img.size
            out.parent.mkdir(parents=True, exist_ok=True)
            img.crop((0, URL_BAR_H, w, h - STATUS_H)).save(out)
            ok += 1
            print("golden", rel, flush=True)
        except Exception as e:  # noqa: BLE001
            bad += 1
            print("FALLO", rel, repr(e), flush=True)
            subprocess.run(["pkill", "-x", "luxium"], capture_output=True)
    print(f"\n{ok} goldens nuevos, {skip} ya existian, {bad} fallos")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
