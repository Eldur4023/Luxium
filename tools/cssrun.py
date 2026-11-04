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
import json
import subprocess
import sys
from pathlib import Path

from PIL import Image
import numpy as np

from csscommon import (REPO, TESTS, GOLDEN, TMP, URL_BAR_H, STATUS_H, DEFAULT_SIZE, find_tests,
                       golden_png, golden_json, options, sizes_of, tag)

OUT = TESTS / "out"
BASELINE = TESTS / "BASELINE.txt"
SHOT_PPM = TMP / "cssrun.ppm"
BOXES_JSON = TMP / "cssrun-boxes.json"
STYLES_JSON = TMP / "cssrun-styles.json"
RECT_TOL = 1.0       # px per coordinate (docs/plan-css-tests.md §1)
BOXY = {"block", "flex", "grid", "table", "table-cell", "list-item", "flow-root", "inline-block"}

LUM_DIFF = 24        # anti-aliasing forgiven: per-pixel luminance threshold
PCT_THRESHOLD = 2.0  # % of differing pixels allowed (default)
SOLID_WIN = 9        # "solid region" window: 9x9
SOLID_FRAC = 0.8     # >=80% of the window differing = inside a blob
SOLID_MAX = 50       # pixels inside solid regions allowed (missing elements)
BLOCK = 8            # ink-detector grid
INK_MAX = 80         # blocks with content in one engine and blank in the other


def ink_mismatch(a: Image.Image, b: Image.Image):
    """Bloques 8x8 con contenido en UN motor y vacios en el otro: caza
    texto o elementos desplazados/ausentes; ignora el ruido de rasterizado
    (misma tinta, distinto anti-aliasing)."""
    aa = np.asarray(a).astype(float).mean(axis=2)
    bb = np.asarray(b).astype(float).mean(axis=2)
    h = min(aa.shape[0], bb.shape[0])
    w = min(aa.shape[1], bb.shape[1])
    h -= h % BLOCK
    w -= w % BLOCK
    A = aa[:h, :w].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).mean(axis=(1, 3))
    B = bb[:h, :w].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).mean(axis=(1, 3))
    A_ink, A_blank = A < 232, A > 248      # histéresis: los ambiguos no cuentan
    B_ink, B_blank = B < 232, B > 248
    return int(((A_ink & B_blank) | (A_blank & B_ink)).sum())


COLOR_TOL = 28       # por canal, sobre la media de un bloque 8x8
COLOR_MAX = 25       # bloques con color distinto (texto del color equivocado, etc.)


def color_mismatch(a: Image.Image, b: Image.Image):
    """Bloques 8x8 cuya media de color difiere: caza el color equivocado de
    texto/bordes que la luminancia perdona (p.ej. gris en vez de azul)."""
    aa = np.asarray(a).astype(float)
    bb = np.asarray(b).astype(float)
    h = min(aa.shape[0], bb.shape[0]) // BLOCK * BLOCK
    w = min(aa.shape[1], bb.shape[1]) // BLOCK * BLOCK
    A = aa[:h, :w].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK, 3).mean(axis=(1, 3))
    B = bb[:h, :w].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK, 3).mean(axis=(1, 3))
    return int((np.abs(A - B).max(axis=2) > COLOR_TOL).sum())


def test_options(html: Path):
    """Umbral y solido por test via comentario en el HTML:
    <!-- cssrun: threshold=3.5 solid=80 -->"""
    o = options(html)
    return float(o.get("threshold", PCT_THRESHOLD)), float(o.get("solid", SOLID_MAX))


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


def shot(html: Path, size: str = DEFAULT_SIZE) -> Image.Image:
    if SHOT_PPM.exists():
        SHOT_PPM.unlink()
    BOXES_JSON.unlink(missing_ok=True)
    cmd = [str(REPO / "build" / "luxium"), str(html), "--size", size, "--shot", str(SHOT_PPM),
           "--boxes", str(BOXES_JSON)]
    for cid in options(html).get("click", "").split(","):
        if cid:
            cmd += ["--click", cid]
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=30)
    if r.returncode != 0 or not SHOT_PPM.exists():
        raise RuntimeError(f"shot exit={r.returncode} {r.stderr.strip()[:120]}")
    return Image.open(SHOT_PPM).convert("RGB")


def rect_diff(golden_json: Path):
    """Compara los rects por data-t contra la sonda de Chromium.
    Devuelve (mal, medidos, sin_medir, detalle).  'sin_medir' = cajas del
    oraculo que el motor propio aun no registra (inline, img...)."""
    if not golden_json.exists() or not BOXES_JSON.exists():
        return 0, 0, 0, []
    want = json.loads(golden_json.read_text())
    got = {b["t"]: b for b in json.loads(BOXES_JSON.read_text())}
    bad = measured = unmeasured = 0
    detail = []
    for w in want:
        if w["d"] not in BOXY:
            continue   # inline/none/contents: sin caja propia comparable
        g = got.get(w["t"])
        if g is None:
            unmeasured += 1
            detail.append(f"  sin-medir {w['t']} ({w['d']}) esperado "
                          f"x={w['x']} y={w['y']} w={w['w']} h={w['h']}")
            continue
        measured += 1
        d = {k: g[k] - w[k] for k in "xywh"}
        if any(abs(v) > RECT_TOL for v in d.values()):
            bad += 1
            detail.append(f"  MAL {w['t']}: " + " ".join(
                f"{k}={w[k]:g}->{g[k]:g}" for k in "xywh" if abs(d[k]) > RECT_TOL))
    return bad, measured, unmeasured, detail


def _same_value(a: str, b: str) -> bool:
    if a == b:
        return True
    if a.endswith("px") and b.endswith("px"):
        try:
            return abs(float(a[:-2]) - float(b[:-2])) <= 0.51
        except ValueError:
            return False
    pa, pb = _rgba(a), _rgba(b)
    if pa and pb:   # alpha is stored in 8 bits: 0.3 vs 0.302
        return all(abs(x - y) <= 0.5 for x, y in zip(pa[:3], pb[:3])) and abs(pa[3] - pb[3]) <= 0.005
    return False


def _gam(c):
    import math
    return 12.92 * c if abs(c) <= 0.0031308 else math.copysign(1.055 * abs(c) ** (1 / 2.4) - 0.055, c)


def _to_srgb(space, n):
    """Chromium serialises colours in their own space (oklab(), lab(), color(srgb-linear ...)): to sRGB 0..1, clamped."""
    import math
    if len(n) != 3:
        return None
    if space == "srgb":
        lin = None
        rgb = n
    else:
        if space in ("oklch", "lch"):
            L, C, H = n
            n = [L, C * math.cos(math.radians(H)), C * math.sin(math.radians(H))]
            space = space.replace("ch", "ab")
        if space == "srgb-linear":
            lin = n
        elif space == "oklab":
            L, A, B = n
            l_, m_, s_ = L + 0.3963377774 * A + 0.2158037573 * B, L - 0.1055613458 * A - 0.0638541728 * B, L - 0.0894841775 * A - 1.2914855480 * B
            l, m, s = l_ ** 3, m_ ** 3, s_ ** 3
            lin = [4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                   -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s]
        elif space == "lab":
            L, A, B = n
            e, k = 216 / 24389, 24389 / 27
            fy = (L + 16) / 116; fx = fy + A / 500; fz = fy - B / 200
            x = fx ** 3 if fx ** 3 > e else (116 * fx - 16) / k
            y = fy ** 3 if L > k * e else L / k
            z = fz ** 3 if fz ** 3 > e else (116 * fz - 16) / k
            w = (0.3457 / 0.3585, 1.0, (1 - 0.3457 - 0.3585) / 0.3585)
            X, Y, Z = x * w[0], y * w[1], z * w[2]
            X, Y, Z = (0.9554735 * X - 0.0230985 * Y + 0.0632593 * Z, -0.0283697 * X + 1.0099955 * Y + 0.0210414 * Z,
                       0.0123140 * X - 0.0205077 * Y + 1.3303659 * Z)
            lin = [3.2409699 * X - 1.5373832 * Y - 0.4986108 * Z, -0.9692436 * X + 1.8759675 * Y + 0.0415551 * Z,
                   0.0556301 * X - 0.2039770 * Y + 1.0569715 * Z]
        else:
            return None
        rgb = [_gam(c) for c in lin]
    return [min(1.0, max(0.0, c)) for c in rgb]


def _rgba(v: str):
    """rgb()/rgba()/color(srgb r g b / a) -> [r, g, b, a] (0-255, alpha 0-1); None otherwise.
    Chromium serialises color-mix() results as color(srgb ...)."""
    try:
        if v.startswith("rgba(") or v.startswith("rgb("):
            p = [float(x) for x in v[v.index("(") + 1:-1].split(",")]
            return p + [1.0] if len(p) == 3 else p
        fn, _, body = v.partition("(")
        body = body[:-1]
        if fn == "color":
            space, _, body = body.partition(" ")
        else:
            space = fn
        vals, _, a = body.partition("/")
        nums = [float(x) for x in vals.split()]
        alpha = float(a) if a.strip() else 1.0
        rgb = _to_srgb(space, nums)
        if rgb is not None:
            return [c * 255 for c in rgb] + [alpha]
    except ValueError:
        pass
    return None


def style_diff(html: Path, golden_json: Path, size: str = DEFAULT_SIZE):
    """Estilos computados propios (--styles, sin ventana) contra getComputedStyle
    de Chromium, por data-t.  Verifica la cascada y los valores tipados sin
    mezclar el layout.  Devuelve (n_malos, detalle)."""
    if not golden_json.exists():
        return 0, []
    want = {e["t"]: e["cs"] for e in json.loads(golden_json.read_text()) if "cs" in e}
    if not want:
        return 0, []
    STYLES_JSON.unlink(missing_ok=True)
    subprocess.run([str(REPO / "build" / "luxium"), str(html), "--size", size, "--styles", str(STYLES_JSON)],
                   cwd=REPO, capture_output=True, timeout=30)
    if not STYLES_JSON.exists():
        return 1, ["  estilos: --styles no produjo salida"]
    got = {e["t"]: e["cs"] for e in json.loads(STYLES_JSON.read_text())}
    bad = 0
    detail = []
    # skipstyle=a,b: properties @keyframes animate; --styles has no animation clock, so they are checked by pixels/rects only
    skip = set(options(html).get("skipstyle", "").split(","))
    for t, cs in want.items():
        mine = got.get(t)
        if mine is None:
            continue
        for prop, expected in cs.items():
            if prop in skip:
                continue
            have = mine.get(prop)
            if have is None:
                continue          # propiedad que no volcamos para este elemento
            if not _same_value(have, expected):
                bad += 1
                detail.append(f"  ESTILO {t}.{prop}: chromium={expected!r} nuestro={have!r}")
    return bad, detail


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


def run_one(rel: str, size: str, bless: bool, verbose: bool = False) -> str:
    html = TESTS / rel
    noext = rel[:-len(".html")]
    img = page_area(shot(html, size))
    golden = golden_png(noext, size)
    if bless:
        golden.parent.mkdir(parents=True, exist_ok=True)
        img.save(golden)
        return "BLESS"
    if not golden.exists():
        out = OUT / (tag(noext, size) + ".png")
        out.parent.mkdir(parents=True, exist_ok=True)
        img.save(out)
        return "PENDIENTE"
    th, solid_max = test_options(html)
    gjson = golden_json(noext, size)
    rbad, rmeas, runm, rdetail = rect_diff(gjson)
    sbad, sdetail = style_diff(html, gjson, size) if not options(html).get("click") else (0, [])
    if verbose:
        for line in rdetail + sdetail:
            print(line)
    golden_img = Image.open(golden).convert("RGB")
    pct, bad = pixel_diff(img, golden_img)
    solid = solid_dirty(img, golden_img)
    ink = ink_mismatch(img, golden_img)
    colr = color_mismatch(img, golden_img)
    rects = f"rects {rmeas - rbad}/{rmeas}" + (f" +{runm} sin medir" if runm else "")
    if pct <= th and solid <= solid_max and ink <= INK_MAX and colr <= COLOR_MAX and rbad == 0 and sbad == 0:
        return f"PASS ({pct:.2f}% tinta={ink}; {rects})"
    motivo = []
    if sbad:
        motivo.append(f"estilos={sbad}")
    if rbad:
        motivo.append(rects)
    if colr > COLOR_MAX:
        motivo.append(f"color={colr}>{COLOR_MAX}")
    if pct > th:
        motivo.append(f"{pct:.2f}%>{th}%")
    if solid > solid_max:
        motivo.append(f"solido={solid}>{solid_max:.0f}")
    if ink > INK_MAX:
        motivo.append(f"tinta={ink}>{INK_MAX}")
    return f"FAIL ({', '.join(motivo)})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--bless", action="store_true",
                    help="reescribe goldens con NUESTRO render (solo oráculo!)")
    ap.add_argument("--write-baseline", action="store_true",
                    help="guarda en css-tests/BASELINE.txt los tests que pasan ahora")
    ap.add_argument("--check-baseline", action="store_true",
                    help="exit 1 si un test de BASELINE.txt deja de pasar")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="detalle de rects por elemento")
    ap.add_argument("targets", nargs="*")
    args = ap.parse_args()

    tests = find_tests()
    rels = [str(p.relative_to(TESTS)) for p in tests]
    if args.list:
        print("\n".join(rels))
        return 0
    if args.targets:
        sel = []
        for t in args.targets:
            t = t.rstrip("/")
            if (TESTS / t).is_dir():
                sel += [r for r in rels if r.startswith(t + "/")]
            else:
                sel.append(t if t.endswith(".html") else t + ".html")
        rels = sel

    OUT.mkdir(exist_ok=True)
    counts = {"PASS": 0, "FAIL": 0, "PENDIENTE": 0, "ERROR": 0, "BLESS": 0}
    failed = []
    results = {}
    for rel in rels:
        noext = rel[:-len(".html")]
        for size in sizes_of(TESTS / rel):
            tid = tag(noext, size)
            try:
                verdict = run_one(rel, size, args.bless, args.verbose)
            except Exception as e:  # noqa: BLE001 -- the runner never dies for one test
                verdict = f"ERROR ({e})"
            key = verdict.split()[0]
            results[tid] = verdict
            counts[key] = counts.get(key, 0) + 1
            print(f"{verdict:<22} {tid}")
            if key in ("FAIL", "ERROR"):
                failed.append(tid)
    passed = sorted(r for r, v in results.items() if v.startswith("PASS"))
    if args.write_baseline:
        BASELINE.write_text("".join(f"{r}\n" for r in passed))
        print(f"\nBASELINE.txt: {len(passed)} tests")
    if args.check_baseline:
        want = BASELINE.read_text().split() if BASELINE.exists() else []
        broke = [r for r in want if r in results and not results[r].startswith("PASS")]
        if broke:
            print("\nREGRESION: pasaban en BASELINE.txt y ya no:")
            for r in broke:
                print("  ", r)
            return 1
    print(f"\n{counts['PASS']} pass, {counts['FAIL']} fail, "
          f"{counts['PENDIENTE']} pendientes (sin golden), "
          f"{counts['ERROR']} error")
    return 1 if failed and not args.check_baseline else 0


if __name__ == "__main__":
    sys.exit(main())
