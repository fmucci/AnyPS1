#!/usr/bin/env python3
"""Finds the translated function range that makes a scenario fail, using the interpreter tier as the reference (finding 0004, 0009).
Everything outside [lo, hi) is left to the interpreter (PS1_NOCODE), the range inside is translated to C; a run is "bad" when it ends in a fatal stop other than the tick limit.
usage: python3 -I tools/bisect_codegen.py "SCENARIO ARGS for build/ps1_game" [--lo HEX] [--hi HEX]
Binary search: smallest hi such that [lo, hi) still fails, then largest lo such that [lo, hi) still fails (each step = one build and one run)."""
import os
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
args = sys.argv[1].split()
LO = int(sys.argv[sys.argv.index("--lo") + 1], 16) if "--lo" in sys.argv else 0x80010000
HI = int(sys.argv[sys.argv.index("--hi") + 1], 16) if "--hi" in sys.argv else 0x801EE000
BASE, END = 0x80010000, 0x801EE000


def bad(lo, hi):
    parts = []
    if lo > BASE:
        parts.append("%x-%x" % (BASE, lo))
    if hi < END:
        parts.append("%x-%x" % (hi, END))
    env = dict(os.environ, PS1_NOCODE=",".join(parts), PS1_GAME_OUT="ps1_game_bisect")        # never overwrites build/ps1_game
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools/recomp/build_game.py"), "-O1"], env=env, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    r = subprocess.run([os.path.join(ROOT, "build/ps1_game_bisect"), __import__("tools.recomp.gamecfg", fromlist=["x"]).DISC] + args, cwd=ROOT, capture_output=True, text=True, timeout=3000)
    msg = [l for l in (r.stdout + r.stderr).splitlines() if "FATAL" in l]
    fail = bool(msg) and "tick limit" not in msg[0]
    print("range %08x-%08x: %s %s" % (lo, hi, "BAD" if fail else "ok", msg[0][:100] if msg else ""), flush=True)
    return fail


def main():
    lo, hi = LO, HI
    if not bad(lo, hi):
        sys.exit("the full range does not fail")
    a, b = lo, hi                                   # smallest hi
    while b - a > 0x400:
        m = ((a + b) // 2) & ~3
        if bad(lo, m):
            b = m
        else:
            a = m
    hi = b
    a, b = lo, hi                                   # largest lo
    while b - a > 0x400:
        m = ((a + b) // 2) & ~3
        if bad(m, hi):
            a = m
        else:
            b = m
    lo = a
    print("minimal failing range: %08x-%08x" % (lo, hi))


if __name__ == "__main__":
    main()
