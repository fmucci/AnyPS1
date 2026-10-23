#!/usr/bin/env python3
"""Relocate a `PS-X EXR` overlay offline by running the GAME'S OWN relocator (queue P2-5; decisions/0002 section 3.8, findings 0002/0013).

usage: python3 -I tools/recomp/exr.py OVERLAY.EXR HOST.EXE BASE_HEX OUT.exe RELOCATOR_HEX [SCRATCH_HEX]      (OUT.exe = a synthetic PS-X EXE that funcs.py / gen.py accept; goes to build/)

The relocation algorithm (a function of the host executable, found by static analysis, its address is RELOCATOR_HEX in game/game.json: opcode dispatch, `jal/j` target rewrite, HI/LO pairing with carry) is executed unchanged
by the oracle on the real data instead of being re-implemented: the result is what the original loader would produce for an image allocated at
BASE. Layout (finding 0002): header 0x800 | relocation area (hdr+0x88 entries, area size (hdr88 & 0x3ffffe00)*4+0x800) | image (hdr+0x1c bytes).
The header from +0x10 is an exec struct: +0x00 entry, +0x04 gp, +0x08 base (written), +0x18 memfill start (+base), +0x20/+0x24 zeroed.
The relocator returns (errors << 16) | warnings; any error aborts.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.recomp import oracle  # noqa: E402

RELOCATOR = None                # address of the host executable's relocator function (game/game.json "relocator")
SCRATCH = 0x801F0000            # header + relocation area are copied here (must lie outside the host image; game/game.json "scratch"); bases must end below it


def parse(path):
    d = open(path, "rb").read()
    if d[:8] != b"PS-X EXR":
        raise ValueError("not a PS-X EXR")
    t_size = struct.unpack_from("<I", d, 0x1C)[0]
    count = struct.unpack_from("<I", d, 0x88)[0]
    bss = struct.unpack_from("<I", d, 0x8C)[0]
    area = (count & 0x3FFFFE00) * 4 + 0x800
    if 0x800 + area + t_size != len(d):
        raise ValueError("file size does not match the layout")
    return {"header": d[:0x800], "relocs": d[0x800:0x800 + 4 * count], "area": area, "count": count, "t_size": t_size,
            "mem_size": bss + 8, "image": d[0x800 + area:], "entry": struct.unpack_from("<I", d, 0x10)[0], "gp": struct.unpack_from("<I", d, 0x14)[0]}


def relocate(exr, fb2_path, base):
    """Returns (relocated image bytes, entry address, errors, warnings)."""
    if base % 4 or base < 0x80000000 or base + exr["mem_size"] > SCRATCH:
        raise ValueError("bad overlay base")
    o = oracle.Oracle()
    o.load_exe(fb2_path)                                   # only for the relocator's code; nothing of the host is executed otherwise
    mem = o.mem
    mem.quiet = True
    hdr = SCRATCH
    rel = SCRATCH + 0x1000
    for k, b in enumerate(exr["header"]):
        mem.store(hdr + k, 1, b)
    for k, b in enumerate(exr["relocs"]):
        mem.store(rel + k, 1, b)
    for k, b in enumerate(exr["image"]):
        mem.store(base + k, 1, b)
    mem.quiet = False
    r = o.st.r
    r[4], r[5], r[6], r[7] = hdr + 0x10, rel, exr["count"], base
    r[31] = oracle.SENTINEL
    o.st.pc = RELOCATOR
    res = o.run(20_000_000)
    if res != "returned":
        raise RuntimeError("relocator did not return: " + res)
    ret = r[2]
    errors, warnings = ret >> 16, ret & 0xFFFF
    if errors:
        raise RuntimeError("the game's relocator reported %d errors" % errors)
    img = bytes(mem.load(base + k, 1) for k in range(exr["t_size"]))
    entry = mem.load(hdr + 0x10, 4)
    return img, entry, errors, warnings


def write_exe(path, image, entry, gp, base):
    hdr = bytearray(0x800)
    hdr[:8] = b"PS-X EXE"
    struct.pack_into("<IIII", hdr, 0x10, entry, gp, base, len(image))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    open(path, "wb").write(bytes(hdr) + image)


def main():
    global RELOCATOR, SCRATCH
    exr_path, fb2, base, out = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), sys.argv[4]
    RELOCATOR = int(sys.argv[5], 16)
    if len(sys.argv) > 6:
        SCRATCH = int(sys.argv[6], 16)
    exr = parse(exr_path)
    img, entry, err, warn = relocate(exr, fb2, base)
    write_exe(out, img, entry, exr["gp"], base)
    print("relocated %d entries at %08x: entry %08x, %d errors, %d warnings, image %d bytes, run-time size %d" % (
        exr["count"], base, entry, err, warn, len(img), exr["mem_size"]))


if __name__ == "__main__":
    main()
