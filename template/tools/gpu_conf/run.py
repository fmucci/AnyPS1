#!/usr/bin/env python3
"""Feed the conformance battery to our GPU model and to an independent emulator (tools/ref_emu host + local core) and compare the display area per cell.

usage: python3 -I tools/gpu_conf/run.py [--ours-only]          (needs build/gpu_conf from gen.py; the emulator part needs work/emu/ built as in tools/ref_emu/README.md)
Frames are game-independent synthetic images; they are written under build/gpu_conf/ (git-ignored)."""
import ctypes
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT)
from tools.recomp import oracle  # noqa: E402

OUT = os.path.join(ROOT, "build", "gpu_conf")
CH, COLS, ROWS = 60, 10, 8


def ours():
    lib = oracle.load_io_lib()
    lib.io_vram.restype = ctypes.POINTER(ctypes.c_uint16)
    lib.io_vram.argtypes = [ctypes.c_void_p]
    ram = (ctypes.c_uint8 * 0x200000)()
    x = lib.io_new(ctypes.addressof(ram))
    ok = ctypes.c_int(1)
    for line in open(os.path.join(OUT, "words.txt")):
        reg, w = (int(t, 16) for t in line.split())
        lib.io_write(x, reg, 4, w, ctypes.byref(ok))
        if not ok.value:
            raise SystemExit("our model rejected %08x -> %08x: %s" % (reg, w, lib.io_last_error(x)))
    v = lib.io_vram(x)
    return [[v[y * 1024 + xx] for xx in range(640)] for y in range(480)]


def emu():
    ppm = os.path.join(OUT, "emu.ppm")
    host = os.path.join(ROOT, "work/emu/host/host")
    core = os.path.join(ROOT, "work/emu/pcsx_rearmed/pcsx_rearmed_libretro.dylib")
    subprocess.run([host, core, os.path.join(OUT, "gpu_conf.exe"), "--frames", "200", "--dump", "199:" + ppm], stderr=subprocess.DEVNULL, cwd=ROOT, timeout=300)
    d = open(ppm, "rb").read().split(b"\n", 3)
    w, h = map(int, d[1].split())
    px = d[3]
    return w, h, [[tuple(px[(y * w + xx) * 3:(y * w + xx) * 3 + 3]) for xx in range(w)] for y in range(h)]


def main():
    o = ours()
    open(os.path.join(OUT, "ours.ppm"), "wb").write(b"P6\n640 480\n255\n" + bytes(c for row in o for p in row for c in (((p & 31) << 3) | ((p & 31) >> 2), (((p >> 5) & 31) << 3) | (((p >> 5) & 31) >> 2), (((p >> 10) & 31) << 3) | (((p >> 10) & 31) >> 2))))
    if "--ours-only" in sys.argv:
        return
    w, h, e = emu()
    print("emulator frame %dx%d" % (w, h))
    names = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "cells.txt")).read().split("\n") if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), "cells.txt")) else []
    tot_same = tot = 0
    for cy in range(ROWS):
        line = []
        for cx in range(COLS):
            same = n = 0
            for y in range(cy * CH, cy * CH + CH):
                for x in range(cx * 64, cx * 64 + 64):
                    p = o[y][x]
                    r, g, b = p & 31, (p >> 5) & 31, (p >> 10) & 31
                    q = e[y][x]
                    n += 1
                    same += (q[0] >> 3, q[1] >> 3, q[2] >> 3) == (r, g, b)
            tot_same += same; tot += n
            line.append("%4d" % (n - same))
        print("row %d (mismatching pixels per cell): %s" % (cy, " ".join(line)))
    print("identical pixels %.2f%%" % (100.0 * tot_same / tot))


main()
