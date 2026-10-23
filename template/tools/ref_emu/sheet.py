#!/usr/bin/env python3
"""contact sheet of PPM pictures, written as a PNG (pure Python: no sips / ImageMagick needed): sheet.py OUT.png COLS DIV file1.ppm file2.ppm ...
DIV = integer down-scale (1 = full size, 2 = half ...). Pictures are 320x240-ish; an agent reads the PNG with its image tool to see what the game shows."""
import struct
import sys
import zlib


def rd(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    out, cols, div, fs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
    W, H = 320 // div, 240 // div
    rows = (len(fs) + cols - 1) // cols
    buf = bytearray(cols * W * rows * H * 3)
    for n, f in enumerate(fs):
        w, h, px = rd(f)
        ox, oy = (n % cols) * W, (n // cols) * H
        for y in range(0, min(h, 240), div):
            for x in range(0, min(w, 320), div):
                s = (y * w + x) * 3
                d = ((oy + y // div) * (cols * W) + ox + x // div) * 3
                buf[d:d + 3] = px[s:s + 3]
    write_png(out, cols * W, rows * H, buf)


if __name__ == "__main__":
    main()
