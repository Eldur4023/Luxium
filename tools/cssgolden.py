#!/usr/bin/env python3
# cssgolden.py -- generates the goldens of css-tests from the ORACLE: every
# page is rendered by Chromium (CEF off-screen, --cefshot) and its page area is
# saved as css-tests/golden/<id>.png, together with the probe's JSON (rects and
# computed styles per [data-t], --cefprobe).
#
# Needs an X11 session (DISPLAY=:0); the window is hidden.
#   python3 tools/cssgolden.py                  # everything missing
#   python3 tools/cssgolden.py --force box flex # regenerate those
import argparse
import os
import subprocess
import sys

from PIL import Image

from csscommon import (REPO, TESTS, GOLDEN, TMP, URL_BAR_H, STATUS_H, find_tests,
                       golden_png, golden_json, oracle_source, sizes_of)

PPM = TMP / "cssgolden.ppm"
PROBE = TMP / "cssgolden.json"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true", help="regenerate existing goldens")
    ap.add_argument("targets", nargs="*")
    args = ap.parse_args()

    rels = [str(p.relative_to(TESTS)) for p in find_tests()]
    if args.targets:
        wanted = []
        for t in args.targets:
            base = t[:-len(".html")] if t.endswith(".html") else t
            wanted += [r for r in rels if r == base + ".html" or r.startswith(base.rstrip("/") + "/")]
        rels = wanted

    GOLDEN.mkdir(exist_ok=True)
    ok = skip = bad = 0
    for rel in rels:
        html = TESTS / rel
        noext = rel[:-len(".html")]
        for size in sizes_of(html):
            png, js = golden_png(noext, size), golden_json(noext, size)
            if png.exists() and js.exists() and not args.force:
                skip += 1
                continue
            try:
                PROBE.unlink(missing_ok=True)
                proc = subprocess.Popen(
                    [str(REPO / "build" / "luxium"), str(oracle_source(html)), "--size", size,
                     "--cefshot", str(PPM), "--cefprobe", str(PROBE)],
                    cwd=REPO,
                    env={**os.environ, "LUXIUM_SCALE": "1"},   # the oracle at 1x even on a HiDPI desktop
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    if proc.wait(timeout=60) != 0:
                        raise RuntimeError(f"luxium exited {proc.returncode}")
                except subprocess.TimeoutExpired:
                    proc.kill()   # only the process this script launched, never the user's running host
                    raise
                img = Image.open(PPM).convert("RGB")
                w, h = img.size
                png.parent.mkdir(parents=True, exist_ok=True)
                img.crop((0, URL_BAR_H, w, h - STATUS_H)).save(png)
                if not PROBE.exists():
                    raise RuntimeError("the probe produced no JSON")
                js.write_text(PROBE.read_text())
                ok += 1
                print("golden", rel, size, flush=True)
            except Exception as e:  # noqa: BLE001
                bad += 1
                print("FAILED", rel, size, repr(e), flush=True)
    print(f"\n{ok} new goldens, {skip} already there, {bad} failures")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
