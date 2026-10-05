#!/usr/bin/env python3
# cssref.py -- reftests: css-tests/ref/NAME.html must render PIXEL-IDENTICAL to
# css-tests/ref/NAME-ref.html with Luxium's own engine.  Both pages say the same
# thing in two different ways (margin:auto vs explicit margins, grid vs flex,
# calc() vs a literal...), so a difference is a bug in one of the two code paths
# -- no Chromium involved, no cross-engine antialiasing noise.
#
#   python3 tools/cssref.py            # all pairs
#   python3 tools/cssref.py NAME       # one
import subprocess
import sys

import numpy as np
from PIL import Image

from csscommon import REPO, TESTS, TMP, URL_BAR_H, STATUS_H

REF = TESTS / "ref"


def shot(page, out):
    out.unlink(missing_ok=True)
    subprocess.run([str(REPO / "build" / "luxium"), str(page), "--shot", str(out)],
                   cwd=REPO, capture_output=True, timeout=30, check=True)
    img = Image.open(out).convert("RGB")
    w, h = img.size
    return np.asarray(img.crop((0, URL_BAR_H, w, h - STATUS_H))).astype(int)


def main():
    names = sys.argv[1:] or sorted(p.stem for p in REF.glob("*.html") if not p.stem.endswith("-ref"))
    bad = 0
    for n in names:
        try:
            a = shot(REF / f"{n}.html", TMP / "ref-a.ppm")
            b = shot(REF / f"{n}-ref.html", TMP / "ref-b.ppm")
            diff = int((np.abs(a - b).max(axis=2) > 0).sum())
            ok = diff == 0
        except Exception as e:  # noqa: BLE001
            print(f"ERROR {n}: {e}")
            bad += 1
            continue
        print(f"{'PASS' if ok else 'FAIL'}  {n}" + ("" if ok else f"  ({diff} pixels differ)"))
        bad += not ok
    print(f"\n{len(names) - bad} pass, {bad} fail")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
