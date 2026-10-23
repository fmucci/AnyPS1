#!/usr/bin/env python3
"""Find functions of one PS-X EXE inside other PS-X EXEs by a masked instruction signature.
usage: python3 -I tools/lib_match.py REF.EXE OTHER.EXE [OTHER.EXE ...] -- name:0xADDR [name:0xADDR ...] [--words N]

Signature = the first N (default 24) instruction words of the reference function with everything address-dependent masked:
  j/jal -> opcode only; lui -> opcode+register; every other I-type instruction whose base/source register is not $sp,
  $zero or the same register as the destination of a previous lui is NOT specially treated: ALL 16-bit immediates of
  loads/stores/addiu/ori are masked unless the base register is $sp (stack offsets are layout-independent).
A match means "same code up to relocation", not "same behaviour"; short functions (< N words) produce a signature that
includes the neighbouring code, so check the function length before trusting a mismatch. Linear scan; no control flow.
"""
import struct
import sys

IMM_OPS = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E}


def load(path):
    d = open(path, "rb").read()
    assert d[:8] == b"PS-X EXE", path
    pc, gp, base, size = struct.unpack("<IIII", d[0x10:0x20])
    n = size // 4
    return base, struct.unpack("<%dI" % n, d[0x800:0x800 + 4 * n])


def mask(w):
    op = w >> 26
    if op in (2, 3):
        return op << 26
    if op == 0x0F:
        return w & 0xFFFF0000
    if op in IMM_OPS and ((w >> 21) & 31) != 29:
        return w & 0xFFFF0000
    if op in (4, 5, 6, 7, 1):  # branches: keep register fields, mask offset
        return w & 0xFFFF0000
    return w


def main():
    a = sys.argv[1:]
    k = a.index("--")
    ref, others, funcs = a[0], a[1:k], a[k + 1:]
    nw = 24
    if "--words" in funcs:
        i = funcs.index("--words")
        nw = int(funcs[i + 1])
        funcs = funcs[:i] + funcs[i + 2:]
    rbase, rw = load(ref)
    print("| function (%s) | %s |" % (ref.split("/")[-1], " | ".join(o.split("/")[-1] for o in others)))
    print("|---|" + "---|" * len(others))
    others_l = [load(o) for o in others]
    masked = [[mask(x) for x in w] for _, w in others_l]
    for f in funcs:
        name, addr = f.split(":")
        i = (int(addr, 16) - rbase) // 4
        sig = [mask(x) for x in rw[i:i + nw]]
        cells = []
        for (b, w), mw in zip(others_l, masked):
            hits = [b + 4 * j for j in range(len(mw) - nw + 1) if mw[j] == sig[0] and mw[j:j + nw] == sig]
            cells.append(", ".join("0x%08x" % h for h in hits[:3]) or "-")
        print("| %s %s | %s |" % (name, addr, " | ".join(cells)))


if __name__ == "__main__":
    main()
