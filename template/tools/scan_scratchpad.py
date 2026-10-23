#!/usr/bin/env python3
"""Scratchpad (0x1f800000..0x1f8003ff) access map of a PS-X EXE, grouped by function-start guess.  usage: python3 -I tools/scan_scratchpad.py EXE
`lui rX,0x1f80` followed within 12 words by a load/store whose base is rX (same window and limits as scan_psx.py;
function start = nearest preceding `addiu sp,sp,-N` or word after `jr ra`+delay slot; accesses through a base register that
was derived from the scratchpad base (rX+offset passed to callees, or a saved pointer) are NOT seen)."""
import bisect
import struct
import sys
from collections import defaultdict

LOAD = {0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu"}
STORE = {0x28: "sb", 0x29: "sh", 0x2B: "sw"}


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


d = open(sys.argv[1], "rb").read()
pc, gp, base, size = struct.unpack("<IIII", d[0x10:0x20])
n = size // 4
w = struct.unpack("<%dI" % n, d[0x800:0x800 + 4 * n])
starts = sorted(set(i for i in range(n) if (w[i] >> 16) == 0x27BD and (w[i] & 0x8000)) | set(i + 2 for i in range(n - 2) if w[i] == 0x03E00008))
per = defaultdict(lambda: defaultdict(lambda: [0, 0]))
for i in range(n):
    if (w[i] >> 26) == 0x0F and (w[i] & 0xFFFF) == 0x1F80:
        r = (w[i] >> 16) & 31
        for k in range(1, 13):
            if i + k >= n:
                break
            x = w[i + k]
            op = x >> 26
            if op == 0x0F and ((x >> 16) & 31) == r:
                break
            if (op in LOAD or op in STORE) and ((x >> 21) & 31) == r:
                off = sx(x & 0xFFFF)
                if 0 <= off < 0x400:
                    j = bisect.bisect_right(starts, i) - 1
                    f = base + 4 * starts[j] if j >= 0 else 0
                    per[f][off][0 if op in LOAD else 1] += 1
print("| function (guess) | offset: loads/stores ... |\n|---|---|")
tot = defaultdict(lambda: [0, 0])
for f in sorted(per):
    cells = []
    for off in sorted(per[f]):
        l, s = per[f][off]
        cells.append("0x%03x:%d/%d" % (off, l, s))
        tot[off][0] += l
        tot[off][1] += s
    print("| 0x%08x | %s |" % (f, " ".join(cells)))
print("\ntotal per offset (loads/stores): " + " ".join("0x%03x:%d/%d" % (o, v[0], v[1]) for o, v in sorted(tot.items())))
print("functions touching the scratchpad: %d" % len(per))
