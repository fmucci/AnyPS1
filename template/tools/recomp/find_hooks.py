#!/usr/bin/env python3
"""Recognise SDK library functions in an executable by their masked signature (data/sdk_signatures.csv, grown by every port) and print game/hooks.csv rows for them.
usage: python3 -I tools/recomp/find_hooks.py [EXE ...] [--write]      (default: every executable of game/game.json; --write appends rows missing from game/hooks.csv)
Signature = sha256 prefix of the first 12 instruction words with relocation-dependent fields masked (hooks.signature); the SDK build differs between titles, so an unrecognised function
is normal for a new SDK version: identify it by hand (docs 04/12), then add its signature to data/sdk_signatures.csv so the next title benefits."""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)
from tools.recomp import funcs, gamecfg, hooks  # noqa: E402

db = {}
for line in open(os.path.join(ROOT, "data", "sdk_signatures.csv")).read().splitlines()[1:]:
    name, p, p2, sig, seen = line.split(",")
    db.setdefault(sig, []).append((name, p, p2))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
exes = args or [e for e, _p in gamecfg.EXES]
out = []
for exe in exes:
    path = exe if os.path.exists(exe) else os.path.join(gamecfg.DISC_DIR, exe)
    img = funcs.Image(path)
    n = hooks.SIG_WORDS
    for i in range(0, len(img.words) - n):
        s = hooks.signature(img.words[i:i + n])
        for name, p, p2 in db.get(s, ()):
            out.append("%s,0x%08x,%s,%s,%s,%s" % (os.path.basename(path), img.base + 4 * i, name, p, p2, s))
for r in out:
    print(r)
print("# %d function(s) recognised in %s" % (len(out), ", ".join(os.path.basename(e) for e in exes)), file=sys.stderr)
if "--write" in sys.argv:
    have = set(open(gamecfg.HOOKS_CSV).read().splitlines())
    with open(gamecfg.HOOKS_CSV, "a") as f:
        for r in out:
            if r not in have:
                f.write(r + "\n")
