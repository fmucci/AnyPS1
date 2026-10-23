#!/usr/bin/env python3
"""Count load-delay-slot hazards in PS-X EXEs.  usage: python3 -I tools/scan_load_delay.py EXE [EXE ...]
R3000 (MIPS-I): the instruction right after a load must not rely on the loaded register (it still sees the OLD value).
This lists places where the instruction following `lb/lbu/lh/lhu/lw/lwl/lwr/mfc0/mfc2/cfc2/lwc2`-style loads reads the load's
destination register as a source: code that either relies on the old value (a real delay-slot dependency) or is data decoded as code.
Linear scan, no control flow: a load in a branch delay slot and a branch target right after a load are not distinguished."""
import struct
import sys


def srcs(w):
    op = w >> 26
    rs, rt = (w >> 21) & 31, (w >> 16) & 31
    if w == 0:
        return set()
    if op == 0:
        f = w & 0x3F
        if f in (0x00, 0x02, 0x03):
            return {rt}
        if f in (0x08, 0x09, 0x11, 0x13):
            return {rs}
        if f in (0x10, 0x12):
            return set()
        return {rs, rt}
    if op in (2, 3, 0x0F):
        return set()
    if op in (4, 5):
        return {rs, rt}
    if op in (6, 7, 1):
        return {rs}
    if op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):
        return {rs, rt}
    if op in (0x12, 0x10):
        return {rt} if ((w >> 21) & 31) in (4, 6) else set()
    return {rs}


LOADS = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26}
for path in sys.argv[1:]:
    d = open(path, "rb").read()
    pc, gp, base, size = struct.unpack("<IIII", d[0x10:0x20])
    n = size // 4
    w = struct.unpack("<%dI" % n, d[0x800:0x800 + 4 * n])
    hz = []
    for i in range(n - 1):
        x = w[i]
        op = x >> 26
        dest = None
        if op in LOADS:
            dest = (x >> 16) & 31
        elif op == 0x12 and ((x >> 21) & 31) in (0, 2) and not x & (1 << 25):
            dest = (x >> 16) & 31
        elif op == 0x10 and ((x >> 21) & 31) == 0:
            dest = (x >> 16) & 31
        if dest and dest in srcs(w[i + 1]) and w[i + 1] != 0:
            hz.append(base + 4 * i)
    print("%s: %d hazards, first %s" % (path.split("/")[-1], len(hz), ", ".join("0x%08x" % a for a in hz[:6])))
