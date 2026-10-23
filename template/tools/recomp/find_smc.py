#!/usr/bin/env python3
"""Find self-modifying code: run an executable in the oracle and list every store into an instruction word that the recompiler covers.
usage: python3 -I tools/recomp/find_smc.py EXE_NAME [--steps N] [--ticks N]
Output: one line per patched word (address, the original word, the distinct values stored, the storing pcs). The result is turned into
docs/symbols/<exe>.patches.csv by hand-checking (queue P3-5; finding 0023)."""
import os
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, ROOT)
from tools.recomp import funcs, hooks, oracle  # noqa: E402
from tools.recomp import gamecfg  # noqa: E402


def main():
    exe = sys.argv[1]
    steps = int(sys.argv[sys.argv.index("--steps") + 1]) if "--steps" in sys.argv else 60_000_000
    path = os.path.join(gamecfg.DISC_DIR, exe)
    img = funcs.Image(path)
    hook_rows = hooks.load_table(exe)
    ents, bodies = funcs.discover(img, {r[0] for r in hook_rows})
    code = set()
    for b in bodies.values():
        code |= b.seen
    o = oracle.Oracle()
    o.mem.low_ranges.append((0x0, 0x1000))
    for lo, hi, _why in gamecfg.LOW_RAM:
        o.mem.low_ranges.append((lo, hi))
    o.load_exe(path)
    o.code_words = code
    h = hooks.Hle(gamecfg.DISC)
    hooks.install(o, h, exe, img)
    hooks.install_bios(o, h, img)
    o.attach_hw(gamecfg.DISC)
    h.card = o.io.card_new(os.path.join(tempfile.mkdtemp(), "card.mcr").encode())
    try:
        res = o.run(steps)
    except oracle.Fatal as e:
        res = "stopped: %s" % e
    print(exe, res, "| steps", o.steps)
    sites = {}
    for pc, addr, size, v in o.smc:
        sites.setdefault(addr, []).append((pc, size, v))
    for addr in sorted(sites):
        orig = img.w(addr & ~3) if img.inside(addr & ~3) else None
        vals = sorted({v for _, _, v in sites[addr]})
        print("  %08x original %s stored %s (size %s) by %s" % (addr, "%08x" % orig if orig is not None else "?", ["%08x" % v for v in vals],
              sorted({s for _, s, _ in sites[addr]}), sorted({"%08x" % p for p, _, _ in sites[addr]})))
    if not sites:
        print("  no store into covered code")


if __name__ == "__main__":
    main()
