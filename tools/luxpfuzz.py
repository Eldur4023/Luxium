#!/usr/bin/env python3
"""Fuzz the .luxp loader: a forged .luxp must be REJECTED or run safely,
never crash the browser.

Packs examples/spa.html, then N times: flips random bytes anywhere in the
file or inside one section (container, document, stylesheet, program), re-wraps it, opens it
(with some clicks) and renders a frame: decoding, cascade, layout, paint.  Any signal / sanitizer report
is a failure.

    python3 tools/luxpfuzz.py [N] [browser]
    browser defaults to build/luxium; point it at an ASan build
    (-fsanitize=address -D_GLIBCXX_SANITIZE_VECTOR=1) to catch silent
    out-of-bounds reads too.
"""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import luxpfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# .luxp files are produced by Lux (lux pack); Luxium only opens them.
LUXC = os.environ.get("LUX", os.path.join(ROOT, "..", "Lux", "build", "lux"))
N = int(sys.argv[1]) if len(sys.argv) > 1 else 300
BROWSER = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "luxium")
CLICKS = ["nav-tasks", "add", "task-1", "del-2", "f0", "th-dark", "acc-green", "sw-dense", "boost"]


rng = random.Random(1234)
with tempfile.TemporaryDirectory() as tmp:
    good = os.path.join(tmp, "good.luxp")
    subprocess.run([LUXC, "pack",
                    os.path.join(ROOT, "examples", "spa.html"), good], check=True, capture_output=True, cwd=ROOT)
    container = luxpfile.read(good)
    raw_original = open(good, "rb").read()
    sections = [(i, k) for i in range(len(container["pages"])) for k in ("document", "sheet", "program")]

    crashes, rejected, ran = 0, 0, 0
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", SDL_VIDEODRIVER="dummy")
    shot = os.path.join(tmp, "shot.ppm")
    clicks = [a for c in CLICKS for a in ("--click", c)]
    for i in range(N):
        bad = os.path.join(tmp, "bad.luxp")
        if i % 2 == 0:
            # inside one section's plain bytes (decoders), re-wrapped with or without zlib
            page, key = rng.choice(sections)
            mutated = {"pages": [dict(p) for p in container["pages"]], "resources": container["resources"]}
            data = bytearray(mutated["pages"][page][key])
            for _ in range(rng.choice([1, 1, 2, 4, 16])):
                data[rng.randrange(len(data))] = rng.randrange(256)
            mutated["pages"][page][key] = bytes(data)
            luxpfile.write(bad, mutated, compress=rng.random() < 0.5)
        else:
            # anywhere in the file (container, sizes, compressed streams)
            data = bytearray(raw_original)
            for _ in range(rng.choice([1, 1, 2, 4, 16])):
                data[rng.randrange(len(data))] = rng.randrange(256)
            open(bad, "wb").write(bytes(data))
        p = subprocess.run([BROWSER, bad] + clicks + ["--shot", shot],
                           capture_output=True, text=True, errors="replace", env=env, timeout=120)
        out = p.stdout + p.stderr
        if p.returncode < 0 or p.returncode >= 128 or "Sanitizer" in out:
            crashes += 1
            keep = os.path.join(ROOT, f"luxpfuzz-crash-{i}.luxp")
            os.replace(bad, keep)
            print(f"CRASH #{i} rc={p.returncode} -> {keep}\n{out[-1500:]}")
            continue
        console = subprocess.run([BROWSER, bad, "--headless"], capture_output=True, text=True,
                                 errors="replace", env=env, timeout=120).stdout
        if "rechazado" in console or ".luxp" in console:
            rejected += 1
        else:
            ran += 1

print(f"{N} mutaciones: {rejected} rechazadas, {ran} ejecutadas sin fallo, {crashes} crashes")
sys.exit(1 if crashes else 0)
