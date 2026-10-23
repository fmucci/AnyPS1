#!/usr/bin/env python3
"""Lists code addresses that are built at run time (`lui` + `addiu|ori` into the same register inside discovered code) or stored as data words and point at code that was NOT
emitted as a function: the callbacks and jump-table targets function discovery cannot recognise (no prologue, no `jr ra` before them; findings 0042, 0043). Each line is a candidate
for tools/recomp/roots.csv (or gen.py --auto-roots).
usage: python3 -I tools/recomp/unresolved_pointers.py [--check] EXE...   (e.g. og-files/disc/CREDITS.EXE; hooks.csv and roots.csv rows are honoured; --check exits 1 when a candidate exists)"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.recomp import funcs, hooks  # noqa: E402
from tools.recomp import gamecfg  # noqa: E402


def main():
    total = 0
    for exe in [a for a in sys.argv[1:] if not a.startswith("--")]:
        img = funcs.Image(exe)
        rows = hooks.load_table(os.path.basename(exe))
        roots = []
        rf = gamecfg.ROOTS_CSV
        if os.path.exists(rf):
            for line in open(rf).read().splitlines()[1:]:
                c = line.split(",", 2)
                if len(c) == 3 and c[0] == os.path.basename(exe):
                    roots.append(int(c[1], 16))
        ents, bodies = funcs.discover(img, {r[0] for r in rows}, roots)
        out = funcs.unresolved_code_pointers(img, ents, bodies)
        total += len(out)
        print("%s: %d candidate(s)" % (os.path.basename(exe), len(out)))
        for v, a, n in out:
            print("  %08x  (built/stored at %08x, %d instructions reachable)" % (v, a, n))
    sys.exit(1 if total and "--check" in sys.argv else 0)


main()
