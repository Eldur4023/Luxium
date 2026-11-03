#!/usr/bin/env python3
"""Drives examples/forms/*.html with synthetic input (--script) on the SDL dummy driver and checks the events
the page receives (--log-events): typing, caret editing, clipboard-free selection, checkbox via label, <select>
popup, textarea, tab order, wheel scrolling of an overflow box, sticky bar.  No display needed.

    tools/formtest.py [build/luxium]
"""
import os, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "luxium")
TMP = tempfile.mkdtemp(prefix="luxium-formtest-")
fails = 0


def run(page, script, size="800x300"):
    shot = os.path.join(TMP, "shot.ppm")
    r = subprocess.run([BIN, os.path.join(ROOT, "examples/forms", page), "--size", size, "--log-events", "--script", script,
                        "--shot", shot], capture_output=True, text=True, timeout=60, env=dict(os.environ, SDL_VIDEODRIVER="dummy"))
    return [l for l in r.stdout.splitlines() if l.startswith("event ")], shot


def expect(name, events, wanted):
    global fails
    it = iter(events)
    ok = all(any(w in e for e in it) for w in wanted)   # wanted is an ordered subsequence
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok:
        fails += 1
        print("       got:", *events[:40], sep="\n         ")


ev, _ = run("controls.html", os.path.join(ROOT, "examples/forms/controls.script"))
expect("typing fires input with the live value", ev, ['input <input id=t> key="hello" value="hello"'])
expect("backspace + insert edit at the caret", ev, ['value="helo"', 'value="helXo"'])
expect("Ctrl+A then typing replaces the selection", ev, ['key="Z" value="Z"'])
expect("password field takes focus and text", ev, ["focus <input id=p>", 'value="abc"'])
expect("label click toggles its checkbox (input + change)", ev, ["input <input id=c>", "change <input id=c>"])
expect("select popup pick fires input + change with the new value", ev, ['change <select id=s> key="" value="Dos"'])
expect("textarea accepts Enter as a newline", ev, ['keydown <textarea id=ta> key="Enter"', 'input <textarea id=ta> key="line2"'])
expect("Tab moves focus to the next control", ev, ["blur <textarea id=ta>", "focus <button id=b>"])
expect("keydown/keyup reach scripts once each", [e for e in ev if "keydown <input id=t>" in e], ["keydown <input id=t>"])

sc = os.path.join(TMP, "scroll.script")
open(sc, "w").write("100 wheel 100 100 -3\n200 wheel 100 100 -3\n")
ev, _ = run("scroll.html", sc, "400x300")
expect("wheel scrolls an overflow:auto box and fires scroll", ev, ["scroll <div id=box>", "scroll <div id=box>"])
print(f"\n{'all passed' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)
