#!/usr/bin/env python3
# cssrun.py — runner del catálogo docs/plan-css-tests.md.
#
# Por cada página de css-tests/ renderiza el motor propio (--shot), recorta
# el área de página (barra URL arriba, tira de estado abajo) y la compara
# contra la imagen golden de css-tests/golden/<id>.png.
#
# Los goldens se generan desde el ORÁCULO (Chromium vía CEF, herramienta
# --probe del plan-css.md fase 0). Sin golden el test queda PENDIENTE y su
# render actual se guarda en css-tests/out/<id>.png para inspección.
#
# Uso:
#   python3 tools/cssrun.py                # toda la suite
#   python3 tools/cssrun.py box/box-sizing # un test (ruta relativa a css-tests)
#   python3 tools/cssrun.py --list         # enumerar
#   python3 tools/cssrun.py --bless ID     # NO usar salvo para goldens del oráculo
#
# Veredictos: PASS (<= umbral de píxeles distintos Y sin regiones sólidas
# grandes), FAIL, PENDIENTE (sin golden), ERROR (el shot falló).  Exit 1 solo
# con FAIL o ERROR.

import argparse
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image
import numpy as np

REPO = Path(__file__).resolve().parent.parent
TESTS = REPO / "css-tests"
GOLDEN = TESTS / "golden"
OUT = TESTS / "out"
SHOT_PPM = Path("/tmp/opencode/cssrun.ppm")

URL_BAR_H = 38       # kUrlH
STATUS_H = 20        # tira de estado inferior
LUM_DIFF = 24        # anti-aliasing perdonado: umbral de luminancia por píxel
PCT_THRESHOLD = 2.0  # % de píxeles distintos permitido (por defecto)
SOLID_WIN = 9        # ventana de "region solida": 9x9
SOLID_FRAC = 0.8     # >=80% de la ventana con diff = interior de un blob
SOLID_MAX = 50       # pixeles "solidos" permitidos (elementos ausentes)


def test_options(html: Path):
    """Umbral y solido por test via comentario en el HTML:
    <!-- cssrun: threshold=3.5 solid=80 -->"""
    th, so = PCT_THRESHOLD, SOLID_MAX
    try:
        m = re.search(r"cssrun:\s*(.*)", html.read_text(encoding="utf-8", errors="ignore"))
    except OSError:
        return th, so
    if m:
        d = dict(re.findall(r"(threshold|solid)\s*=\s*([\d.]+)", m.group(1)))
        th = float(d.get("threshold", th))
        so = float(d.get("solid", so))
    return th, so


def solid_dirty(a: Image.Image, b: Image.Image):
    """Pixeles de diff que estan DENTRO de una region grande y solida:
    un elemento ausente o desplazado deja miles; el ruido de antialiasing,
    casi ninguno.  Es el detector de 'falta algo que se ve a simple vista'."""
    aa = np.asarray(a).astype(np.int32)
    bb = np.asarray(b).astype(np.int32)
    dl = np.abs(aa.mean(axis=2) - bb.mean(axis=2)) > LUM_DIFF
    ii = np.cumsum(np.cumsum(dl.astype(np.int32), 0), 1)
    ii = np.pad(ii, ((1, 0), (1, 0)))
    h, w = dl.shape
    r = SOLID_WIN // 2
    ys = np.arange(h)
    xs = np.arange(w)
    y1 = np.minimum(ys + r + 1, h)
    y0 = np.maximum(ys - r, 0)
    x1 = np.minimum(xs + r + 1, w)
    x0 = np.maximum(xs - r, 0)
    s = (ii[np.ix_(y1, x1)] - ii[np.ix_(y0, x1)]
         - ii[np.ix_(y1, x0)] + ii[np.ix_(y0, x0)])
    area = (y1 - y0)[:, None] * (x1 - x0)[None, :]
    return int((dl & (s >= SOLID_FRAC * area)).sum())


def find_tests():
    return sorted(p for p in TESTS.rglob("*.html") if p.is_file())


def shot(html: Path) -> Image.Image:
    if SHOT_PPM.exists():
        SHOT_PPM.unlink()
    r = subprocess.run(
        [str(REPO / "build" / "luxium"), str(html), "--shot", str(SHOT_PPM)],
        cwd=REPO, capture_output=True, text=True, timeout=30)
    if r.returncode != 0 or not SHOT_PPM.exists():
        raise RuntimeError(f"shot exit={r.returncode} {r.stderr.strip()[:120]}")
    return Image.open(SHOT_PPM).convert("RGB")


def page_area(img: Image.Image) -> Image.Image:
    w, h = img.size
    return img.crop((0, URL_BAR_H, w, h - STATUS_H))


def pixel_diff(a: Image.Image, b: Image.Image):
    if a.size != b.size:
        return 100.0, 0
    aa = np.asarray(a).astype(int)
    bb = np.asarray(b).astype(int)
    dl = np.abs(aa.mean(axis=2) - bb.mean(axis=2))
    bad = int((dl > LUM_DIFF).sum())
    pct = 100.0 * bad / dl.size
    return pct, bad


def run_one(rel: str, bless: bool) -> str:
    html = TESTS / rel
    img = page_area(shot(html))
    golden = GOLDEN / (rel[:-len(".html")] + ".png")
    if bless:
        golden.parent.mkdir(parents=True, exist_ok=True)
        img.save(golden)
        return "BLESS"
    if not golden.exists():
        OUT.joinpath(rel[:-len(".html")] + ".png").parent.mkdir(
            parents=True, exist_ok=True)
        img.save(OUT / (rel[:-len(".html")] + ".png"))
        return "PENDIENTE"
    th, solid_max = test_options(html)
    pct, bad = pixel_diff(img, Image.open(golden).convert("RGB"))
    solid = solid_dirty(img, Image.open(golden).convert("RGB"))
    if pct <= th and solid <= solid_max:
        return f"PASS ({pct:.2f}% solido={solid})"
    motivo = []
    if pct > th:
        motivo.append(f"{pct:.2f}%>{th}%")
    if solid > solid_max:
        motivo.append(f"solido={solid}>{solid_max:.0f}")
    return f"FAIL ({', '.join(motivo)})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--bless", action="store_true",
                    help="reescribe goldens con NUESTRO render (solo oráculo!)")
    ap.add_argument("targets", nargs="*")
    args = ap.parse_args()

    tests = find_tests()
    rels = [str(p.relative_to(TESTS)) for p in tests]
    if args.list:
        print("\n".join(rels))
        return 0
    if args.targets:
        rels = [t if t.endswith(".html") else t + ".html" for t in args.targets]

    OUT.mkdir(exist_ok=True)
    counts = {"PASS": 0, "FAIL": 0, "PENDIENTE": 0, "ERROR": 0, "BLESS": 0}
    failed = []
    for rel in rels:
        try:
            verdict = run_one(rel, args.bless)
        except Exception as e:  # noqa: BLE001 — el runner nunca muere por un test
            verdict = f"ERROR ({e})"
        key = verdict.split()[0]
        counts[key] = counts.get(key, 0) + 1
        print(f"{verdict:<22} {rel}")
        if key in ("FAIL", "ERROR"):
            failed.append(rel)
    print(f"\n{counts['PASS']} pass, {counts['FAIL']} fail, "
          f"{counts['PENDIENTE']} pendientes (sin golden), "
          f"{counts['ERROR']} error")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
