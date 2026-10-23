"""Try to emit every function-like region of a PS-X EXE's code range and report what the emitter rejects.
usage: python3 -I tools/recomp/survey.py EXE CODE_START CODE_END      (addresses in hex; the code range comes from findings 0014/0015)

Regions are split after each `jr ra` + delay slot (so leaf functions without a prologue are separate regions; regions that
`j`-jump or fall into each other are still emitted independently). Emission only (no execution). Output: counts and the first
rejected sites. Not a function-discovery tool (that is queue P2-6): it exists to check that the emitter's rules cover real code."""
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.recomp import emit  # noqa: E402


def main():
    path, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    d = open(path, "rb").read()
    pc, gp, base, size = struct.unpack("<IIII", d[0x10:0x20])
    words = struct.unpack("<%dI" % (size // 4), d[0x800:0x800 + size])
    a, b = (lo - base) // 4, (hi - base) // 4
    regions, start = [], a
    i = a
    while i < b:
        if words[i] == 0x03E00008 and i + 1 < b:  # jr ra + delay slot ends a region
            regions.append((start, i + 2))
            start = i + 2
            i += 2
        else:
            i += 1
    if start < b:
        regions.append((start, b))
    ok = 0
    rej = Counter()
    examples = {}
    unchecked = 0
    for s, e in regions:
        ws = list(words[s:e])
        try:
            _, rep = emit.emit_fragment(ws, base + 4 * s, "f")
            ok += 1
            unchecked += len(rep["unchecked_slot_loads"])
        except (emit.HazardError, emit.Unsupported) as ex:
            key = (type(ex).__name__, str(ex).split(":")[0][:60])
            rej[key] += 1
            examples.setdefault(key, (hex(base + 4 * s), str(ex)[:120]))
    print("%s [%s..%s]: regions %d, emitted %d, rejected %d, delay-slot loads with unknown successor %d" % (
        path.split("/")[-1], hex(lo), hex(hi), len(regions), ok, sum(rej.values()), unchecked))
    for k, v in rej.most_common():
        print("   %-12s %-50s x%d   e.g. %s" % (k[0], k[1], v, examples[k]))


if __name__ == "__main__":
    main()
