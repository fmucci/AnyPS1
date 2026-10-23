#!/usr/bin/env python3
"""GPU conformance battery (queue P4-7): a list of (register, word) writes that draws one primitive type/mode per 64x64 cell of VRAM (0..639 x 0..447),
plus a tiny PS-X EXE that replays the list on a console/emulator. The same list is fed to our model (run.py) and to an independent emulator; the display area is compared.

usage: python3 -I tools/gpu_conf/gen.py OUT_DIR      -> OUT_DIR/gpu_conf.exe and OUT_DIR/words.txt (hex register word per line)"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "recomp"))
import asm  # noqa: E402  (our tiny MIPS word builders; no game data)

GP0, GP1 = 0x1F801810, 0x1F801814
W = []


def gp0(w): W.append((GP0, w & 0xFFFFFFFF))
def gp1(w): W.append((GP1, w & 0xFFFFFFFF))
def xy(x, y): return ((y & 0xFFFF) << 16) | (x & 0xFFFF)
def col(r, g, b): return (b << 16) | (g << 8) | r
def uvw(u, v, hi=0): return (hi << 16) | (v << 8) | u


def e1(tpx=0, tpy=0, semi=0, depth=0, dither=0, draw=1): return 0xE1000000 | (tpx & 15) | (tpy << 4) | (semi << 5) | (depth << 7) | (dither << 9) | (draw << 10)


def upload(x, y, w, h, halfwords):
    gp0(0xA0000000); gp0(xy(x, y)); gp0(xy(w, h))
    hw = list(halfwords)
    if len(hw) % 2: hw.append(0)
    for i in range(0, len(hw), 2): gp0(hw[i] | (hw[i + 1] << 16))


def rgb15(r, g, b, s=0): return (r & 31) | ((g & 31) << 5) | ((b & 31) << 10) | (s << 15)


# ---- set-up
gp1(0x00000000)                                    # reset
gp1(0x03000000)                                    # display on
gp1(0x05000000)                                    # display start (0,0)
gp1(0x06C60260)                                    # horizontal range
gp1(0x07040010)                                    # vertical range 16..256 (240 lines; 480 interlaced)
gp1(0x08000027)                                    # 640x480, 15 bit, interlaced
gp0(0xE3000000); gp0(0xE4000000 | (511 << 10) | 1023); gp0(0xE5000000); gp0(0xE6000000)
gp0(e1())
gp0(0x02000000); gp0(xy(0, 0)); gp0(xy(1024, 512))   # clear VRAM

# ---- textures (outside the compared area: x >= 640)
t15 = []
for v in range(64):
    for u in range(64):
        t15.append(0 if (u + v) % 9 == 0 else rgb15((u * 3 + v) % 32, (v * 5 + u // 2) % 32, (u ^ v) % 32, 1 if ((u // 4 + v // 4) % 2) else 0))
upload(640, 0, 64, 64, t15)
t8 = []                                            # 64x64 texels, 2 per halfword -> 32 halfwords per row
for v in range(64):
    for u in range(0, 64, 2): t8.append(((u ^ v) & 0xFF) | ((((u + 1) ^ v) & 0xFF) << 8))
upload(704, 0, 32, 64, t8)
clut8 = [0 if i % 17 == 0 else rgb15(i % 32, (i * 3 // 2) % 32, (255 - i) % 32, 1 if i % 5 == 0 else 0) for i in range(256)]
upload(640, 256, 256, 1, clut8)
t4 = []
for v in range(64):
    for u in range(0, 64, 4):
        h = 0
        for k in range(4): h |= (((u + k + v) // 2) & 15) << (4 * k)
        t4.append(h)
upload(768, 0, 16, 64, t4)
clut4 = [0 if i == 3 else rgb15(i * 2, 31 - i * 2, (i * 7) % 32, 1 if i % 4 == 1 else 0) for i in range(16)]
upload(640, 257, 16, 1, clut4)
CLUT8, CLUT4 = 40 | (256 << 6), 40 | (257 << 6)
TP = {15: (10, 2), 8: (11, 1), 4: (12, 0)}           # texture page x base (in 64 halfword units), depth code


def tpage(depth, semi=0): return TP[depth][0] | (semi << 5) | (TP[depth][1] << 7)


def clut(depth): return 0 if depth == 15 else (CLUT8 if depth == 8 else CLUT4)


# ---- cell battery
cells = []
CH = 60                                            # cell height: 8 rows x 60 = 480 display lines


def cell(fn): cells.append(fn)


PAL = [(col(250, 20, 20), col(20, 250, 20), col(20, 20, 250), col(240, 240, 40)), (col(30, 30, 30), col(220, 220, 220), col(120, 60, 200), col(10, 160, 120)),
       (col(200, 100, 20), col(40, 90, 220), col(255, 255, 255), col(0, 0, 0))]


def backdrop(cx, cy, n):
    p = PAL[n % 3]
    x0, y0 = cx * 64, cy * CH
    gp0(e1(draw=1)); gp0(0x38000000 | (p[0] & 0xFFFFFF)); gp0(xy(x0, y0)); gp0(p[1]); gp0(xy(x0 + 63, y0)); gp0(p[2]); gp0(xy(x0, y0 + CH - 1)); gp0(p[3]); gp0(xy(x0 + 63, y0 + CH - 1))


TRI = [(7, 5), (58, 13), (21, 55)]
QUAD = [(6, 8), (57, 6), (11, 54), (58, 51)]
for dither in (0, 1):
    cell(lambda x0, y0, d=dither: (gp0(e1(dither=d)), gp0(0x20000000 | col(220, 90, 30)), *[gp0(xy(x0 + a, y0 + b)) for a, b in TRI]))
    cell(lambda x0, y0, d=dither: (gp0(e1(dither=d)), gp0(0x30000000 | col(250, 10, 10)), gp0(xy(x0 + 7, y0 + 5)), gp0(col(10, 250, 10)), gp0(xy(x0 + 58, y0 + 13)), gp0(col(10, 10, 250)), gp0(xy(x0 + 21, y0 + 55))))
cell(lambda x0, y0: (gp0(e1()), gp0(0x28000000 | col(40, 200, 200)), *[gp0(xy(x0 + a, y0 + b)) for a, b in QUAD]))
cell(lambda x0, y0: (gp0(e1(dither=1)), gp0(0x38000000 | col(255, 0, 0)), gp0(xy(x0 + 6, y0 + 8)), gp0(col(0, 255, 0)), gp0(xy(x0 + 57, y0 + 6)), gp0(col(0, 0, 255)), gp0(xy(x0 + 11, y0 + 54)), gp0(col(255, 255, 0)), gp0(xy(x0 + 58, y0 + 51))))
for m in range(4):
    cell(lambda x0, y0, m=m: (gp0(e1(semi=m)), gp0(0x22000000 | col(200, 120, 60)), *[gp0(xy(x0 + a, y0 + b)) for a, b in TRI]))
    cell(lambda x0, y0, m=m: (gp0(e1(semi=m, dither=1)), gp0(0x32000000 | col(250, 10, 10)), gp0(xy(x0 + 7, y0 + 5)), gp0(col(10, 250, 10)), gp0(xy(x0 + 58, y0 + 13)), gp0(col(10, 10, 250)), gp0(xy(x0 + 21, y0 + 55))))
    cell(lambda x0, y0, m=m: (gp0(e1(semi=m)), gp0(0x2A000000 | col(100, 200, 100)), *[gp0(xy(x0 + a, y0 + b)) for a, b in QUAD]))
UVT = [(3, 2), (60, 5), (9, 58)]
UVQ = [(2, 3), (61, 4), (5, 60), (58, 57)]
for depth in (15, 8, 4):
    cl = clut(depth)
    def tex_tri(x0, y0, depth=depth, cl=cl, op=0x24, c=0x80, semi=0, gour=False):
        gp0(e1(semi=semi)); gp0((op << 24) | col(c, c, c)); gp0(xy(x0 + TRI[0][0], y0 + TRI[0][1])); gp0(uvw(*UVT[0], cl))
        gp0(xy(x0 + TRI[1][0], y0 + TRI[1][1])); gp0(uvw(*UVT[1], tpage(depth, semi))); gp0(xy(x0 + TRI[2][0], y0 + TRI[2][1])); gp0(uvw(*UVT[2]))
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x25))                        # raw
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x24, c=0x80))                # modulated neutral
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x24, c=0x60))                # modulated dark
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x24, c=0xC0))                # modulated bright (saturates)
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x26, c=0x80, semi=0))        # semi, mode 0
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x25 | 2, semi=1))            # semi raw, mode 1
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x26, c=0x90, semi=2))        # semi, mode 2
    cell(lambda x0, y0, f=tex_tri: f(x0, y0, op=0x26, c=0x90, semi=3))        # semi, mode 3
    def tex_quad_g(x0, y0, depth=depth, cl=cl):
        gp0(e1(dither=1)); gp0(0x3C000000 | col(250, 100, 50)); gp0(xy(x0 + QUAD[0][0], y0 + QUAD[0][1])); gp0(uvw(*UVQ[0], cl))
        gp0(col(60, 250, 90)); gp0(xy(x0 + QUAD[1][0], y0 + QUAD[1][1])); gp0(uvw(*UVQ[1], tpage(depth)))
        gp0(col(90, 60, 250)); gp0(xy(x0 + QUAD[2][0], y0 + QUAD[2][1])); gp0(uvw(*UVQ[2]))
        gp0(col(250, 250, 60)); gp0(xy(x0 + QUAD[3][0], y0 + QUAD[3][1])); gp0(uvw(*UVQ[3]))
    cell(tex_quad_g)
# rectangles
cell(lambda x0, y0: (gp0(e1()), gp0(0x60000000 | col(230, 60, 120)), gp0(xy(x0 + 9, y0 + 11)), gp0(xy(37, 29))))
cell(lambda x0, y0: (gp0(e1()), *[(gp0(0x68000000 | col(255, 255, 255)), gp0(xy(x0 + 5 + 3 * i, y0 + 7 + 2 * i))) for i in range(18)]))
cell(lambda x0, y0: (gp0(e1()), gp0(0x70000000 | col(20, 220, 180)), gp0(xy(x0 + 13, y0 + 9)), gp0(0x78000000 | col(220, 220, 20)), gp0(xy(x0 + 27, y0 + 33))))
for m in range(4):
    cell(lambda x0, y0, m=m: (gp0(e1(semi=m)), gp0(0x62000000 | col(180, 90, 200)), gp0(xy(x0 + 9, y0 + 11)), gp0(xy(37, 29))))
for depth in (15, 8, 4):
    cl = clut(depth)
    cell(lambda x0, y0, depth=depth, cl=cl: (gp0(e1(tpx=TP[depth][0], depth=TP[depth][1])), gp0(0x64000000 | col(0x80, 0x80, 0x80)), gp0(xy(x0 + 8, y0 + 10)), gp0(uvw(5, 7, cl)), gp0(xy(41, 37))))
    cell(lambda x0, y0, depth=depth, cl=cl: (gp0(e1(tpx=TP[depth][0], depth=TP[depth][1])), gp0(0x65000000 | col(0x80, 0x80, 0x80)), gp0(xy(x0 + 8, y0 + 10)), gp0(uvw(5, 7, cl)), gp0(xy(41, 37))))
    cell(lambda x0, y0, depth=depth, cl=cl: (gp0(e1(tpx=TP[depth][0], depth=TP[depth][1])), gp0(0x7C000000 | col(0xB0, 0x60, 0xA0)), gp0(xy(x0 + 12, y0 + 12)), gp0(uvw(9, 11, cl))))
    cell(lambda x0, y0, depth=depth, cl=cl: (gp0(e1(tpx=TP[depth][0], depth=TP[depth][1], semi=1)), gp0(0x66000000 | col(0x80, 0x80, 0x80)), gp0(xy(x0 + 8, y0 + 10)), gp0(uvw(5, 7, cl)), gp0(xy(41, 37))))
# lines
cell(lambda x0, y0: (gp0(e1()), gp0(0x40000000 | col(255, 255, 255)), gp0(xy(x0 + 4, y0 + 6)), gp0(xy(x0 + 59, y0 + 22)), gp0(0x40000000 | col(255, 0, 0)), gp0(xy(x0 + 5, y0 + 50)), gp0(xy(x0 + 57, y0 + 51)), gp0(0x40000000 | col(0, 255, 0)), gp0(xy(x0 + 30, y0 + 4)), gp0(xy(x0 + 33, y0 + 57))))
cell(lambda x0, y0: (gp0(e1(dither=1)), gp0(0x50000000 | col(255, 0, 0)), gp0(xy(x0 + 4, y0 + 6)), gp0(col(0, 0, 255)), gp0(xy(x0 + 59, y0 + 40)), gp0(0x50000000 | col(0, 255, 0)), gp0(xy(x0 + 6, y0 + 56)), gp0(col(255, 255, 0)), gp0(xy(x0 + 58, y0 + 12))))
for m in range(4):
    cell(lambda x0, y0, m=m: (gp0(e1(semi=m)), gp0(0x42000000 | col(200, 100, 50)), gp0(xy(x0 + 4, y0 + 8)), gp0(xy(x0 + 59, y0 + 30)), gp0(0x42000000 | col(50, 200, 100)), gp0(xy(x0 + 8, y0 + 4)), gp0(xy(x0 + 40, y0 + 57))))
cell(lambda x0, y0: (gp0(e1()), gp0(0x48000000 | col(255, 255, 0)), gp0(xy(x0 + 6, y0 + 6)), gp0(xy(x0 + 50, y0 + 14)), gp0(xy(x0 + 12, y0 + 40)), gp0(xy(x0 + 56, y0 + 56)), gp0(0x55555555)))
cell(lambda x0, y0: (gp0(e1()), gp0(0x58000000 | col(255, 0, 0)), gp0(xy(x0 + 6, y0 + 6)), gp0(col(0, 255, 0)), gp0(xy(x0 + 50, y0 + 14)), gp0(col(0, 0, 255)), gp0(xy(x0 + 12, y0 + 40)), gp0(col(255, 255, 255)), gp0(xy(x0 + 56, y0 + 56)), gp0(0x55555555)))
# fill and copy, mask bit
cell(lambda x0, y0: (gp0(0x02000000 | col(40, 120, 240)), gp0(xy(x0 + 16, y0 + 8)), gp0(xy(32, 24))))
cell(lambda x0, y0: (gp0(0x80000000), gp0(xy(x0 - 64, y0)), gp0(xy(x0, y0)), gp0(xy(48, 40))))
cell(lambda x0, y0: (gp0(0xE6000001), gp0(e1()), gp0(0x60000000 | col(255, 255, 255)), gp0(xy(x0 + 8, y0 + 8)), gp0(xy(30, 30)), gp0(0xE6000002), gp0(0x60000000 | col(255, 0, 0)), gp0(xy(x0 + 20, y0 + 20)), gp0(xy(30, 30)), gp0(0xE6000000)))

COLS, ROWS = 10, 8
assert len(cells) <= COLS * ROWS, len(cells)
for i, fn in enumerate(cells):
    cx, cy = i % COLS, i // COLS
    backdrop(cx, cy, i)
    fn(cx * 64, cy * CH)

# the replay program: for each (register, word) pair: store word to register
words = []
for reg, w in W: words += [reg, w]
code = [asm.i("lui", 0, 9, 0x8001), asm.i("ori", 9, 9, 0x0200),            # t1 = table address 0x80010200
        asm.i("lui", 0, 10, len(W) >> 16), asm.i("ori", 10, 10, len(W) & 0xFFFF),   # t2 = pair count
        asm.i("lw", 9, 12, 0), asm.i("lw", 9, 11, 4), asm.i("addiu", 9, 9, 8), asm.i("sw", 12, 11, 0),
        asm.i("addiu", 10, 10, -1), asm.i("bne", 10, 0, -6), asm.nop(),
        asm.j("j", 0x80010000 + 4 * 11), asm.nop()]
exe = bytearray(0x800)
exe[:8] = b"PS-X EXE"
struct.pack_into("<IIII", exe, 0x10, 0x80010000, 0, 0x80010000, 0x200 + 4 * len(words))
struct.pack_into("<II", exe, 0x30, 0x801FFF00, 0)
body = bytearray()
for w in code: body += struct.pack("<I", w)
body += bytes(0x200 - len(body))
for w in words: body += struct.pack("<I", w)
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "gpu_conf.exe"), "wb").write(bytes(exe) + bytes(body))
open(os.path.join(out, "words.txt"), "w").write("\n".join("%08x %08x" % p for p in W) + "\n")
print("cells %d, GPU words %d" % (len(cells), len(W)))
