#!/usr/bin/env python3
"""Raw-word scan of PS-X EXE files for GTE (COP2) instructions. usage: python3 -I tools/scan_gte.py [--min-cluster N] EXE [EXE ...]   (default N=8)

Decodes by hand from the 32-bit words (does NOT rely on Ghidra's decoder, see queue P0-6):
  opcode 0x12 (COP2): bit25=1 -> GTE command, real command = bits 0-5, sf = bit19, lm = bit10,
                      MVMVA fields: matrix bits 17-18, vector 15-16, translation 13-14;
                      bit25=0 -> rs field: 0 MFC2, 2 CFC2, 4 MTC2, 6 CTC2 (rd = GTE register number)
  opcode 0x32 LWC2, 0x3a SWC2 (rt = GTE data register)
Command names: psx-spx "GTE Opcode Summary". Linear scan, data words that look like COP2 are counted too:
clusters (a run of COP2 words with gaps <= 16 words) are printed so false positives show up as isolated hits.
"""
import struct
import sys
from collections import Counter

NAMES = {0x01: "RTPS", 0x06: "NCLIP", 0x0C: "OP", 0x10: "DPCS", 0x11: "INTPL", 0x12: "MVMVA", 0x13: "NCDS",
         0x14: "CDP", 0x16: "NCDT", 0x1B: "NCCS", 0x1C: "CC", 0x1E: "NCS", 0x20: "NCT", 0x28: "SQR",
         0x29: "DCPL", 0x2A: "DPCT", 0x2D: "AVSZ3", 0x2E: "AVSZ4", 0x30: "RTPT", 0x3D: "GPF", 0x3E: "GPL",
         0x3F: "NCCT"}
MOVES = {0: "MFC2", 2: "CFC2", 4: "MTC2", 6: "CTC2"}


def scan(path, minc):
    d = open(path, "rb").read()
    assert d[:8] == b"PS-X EXE", path
    pc, gp, base, size = struct.unpack("<IIII", d[0x10:0x20])
    n = size // 4
    w = struct.unpack("<%dI" % n, d[0x800:0x800 + 4 * n])
    sites = []  # (index, class) class: 'cmd' | 'move' | 'mem'
    for i, x in enumerate(w):
        op = x >> 26
        if op == 0x12:
            if x & (1 << 25):
                sites.append((i, "cmd"))
            elif ((x >> 21) & 31) in MOVES:
                sites.append((i, "move"))
        elif op in (0x32, 0x3A):
            sites.append((i, "mem"))
    clusters, cur = [], None
    for i, _ in sites:
        if cur and i - cur[1] <= 16:
            cur[1] = i
            cur[2] += 1
        else:
            cur = [i, i, 1]
            clusters.append(cur)
    keep = [c for c in clusters if c[2] >= minc]
    inside = lambda i: any(a <= i <= b for a, b, _ in keep)
    cmds, mv, moves, mem = Counter(), Counter(), Counter(), Counter()
    for i, k in sites:
        if not inside(i):
            continue
        x = w[i]
        if k == "cmd":
            cmds[x & 0x3F] += 1
            if x & 0x3F == 0x12:
                mv[((x >> 17) & 3, (x >> 15) & 3, (x >> 13) & 3, (x >> 19) & 1, (x >> 10) & 1)] += 1
        elif k == "move":
            moves[MOVES[(x >> 21) & 31]] += 1
        else:
            mem["LWC2" if x >> 26 == 0x32 else "SWC2"] += 1
    return base, n, sites, clusters, keep, cmds, mv, moves, mem


def main():
    args = sys.argv[1:]
    minc = 8
    if args[:1] == ["--min-cluster"]:
        minc, args = int(args[1]), args[2:]
    for path in args:
        base, n, sites, clusters, keep, cmds, mv, moves, mem = scan(path, minc)
        print("\n## %s (load 0x%08x, %d words)\n" % (path.split("/")[-1], base, n))
        print("raw COP2-class words: %d in %d clusters; kept clusters with >= %d words: %d (%d words)\n" % (
            len(sites), len(clusters), minc, len(keep), sum(c[2] for c in keep)))
        if not keep:
            continue
        print("| command in kept clusters | count |\n|---|---:|")
        unnamed = 0
        for c, k in sorted(cmds.items()):
            if c in NAMES:
                print("| 0x%02x %s | %d |" % (c, NAMES[c], k))
            else:
                unnamed += k
        print("| (command numbers not in psx-spx list) | %d |" % unnamed)
        print("\nMVMVA (matrix, vector, translation, sf, lm)=count: " + "; ".join("%s=%d" % (k, v) for k, v in sorted(mv.items())))
        print("\nmoves: %s; memory: %s" % (dict(moves), dict(mem)))
        print("\nkept clusters: " + "; ".join("0x%08x..0x%08x (%d)" % (base + 4 * a, base + 4 * b, c) for a, b, c in keep))


if __name__ == "__main__":
    main()
