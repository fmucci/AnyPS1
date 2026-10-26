"""What the boot-executable functions that no run entered would do to the hardware: python3 -I tools/exe_unreached.py EXE FUNCS --cov RUN.json [...]
INFERRED from linear decoding of each function's extent (to the next function start): hardware register groups (lui 0x1F80/0x1F801xxx + load/store offset) and BIOS A0/B0/C0 stub numbers.
Compares them with the register groups and BIOS calls that the runs exercised (coverage keys io_read/io_write = (addr-0x1F801000)>>3, bios)."""
import json, struct, sys, collections
a = sys.argv[1:]
exe = open(a[0], "rb").read()
base = struct.unpack("<I", exe[0x18:0x1C])[0]
def word(addr): o = 0x800 + addr - base; return struct.unpack("<I", exe[o:o + 4])[0]
fs = sorted(int(x, 16) for x in open(a[1]).read().split())
cov = set(); io = set()
for k, v in enumerate(a):
    if v == "--cov":
        d = json.load(open(a[k + 1])); cov |= set(d["functions"])
        for g in ("io_read", "io_write"): io |= {int(x) for x in d[g]}
NAMES = {0x1040: "JOY", 0x1050: "SIO", 0x1060: "RAM size", 0x1070: "I_STAT/I_MASK", 0x1080: "DMA0 MDEC in", 0x1090: "DMA1 MDEC out", 0x10A0: "DMA2 GPU", 0x10B0: "DMA3 CD", 0x10C0: "DMA4 SPU", 0x10D0: "DMA5 PIO", 0x10E0: "DMA6 OTC", 0x10F0: "DMA ctrl", 0x1100: "timer0", 0x1110: "timer1", 0x1120: "timer2", 0x1800: "CD", 0x1810: "GPU", 0x1820: "MDEC", 0x1C00: "SPU"}
def name(r):
    best = [n for lo, n in NAMES.items() if lo <= r - 0x1F800000 < lo + (0x10 if lo < 0x1C00 else 0x400)]
    return best[0] if best else "?"
unreached = [f for k, f in enumerate(fs) if f not in cov]
hw = collections.defaultdict(set); bios = collections.defaultdict(set); cop2 = set()
for k, f in enumerate(fs):
    if f in cov: continue
    end = fs[k + 1] if k + 1 < len(fs) else f + 0x400
    lui = {}; ab = None
    for ad in range(f, min(end, f + 0x4000), 4):
        try: x = word(ad)
        except Exception: break
        op = x >> 26
        if op == 0x0F: lui[(x >> 16) & 31] = x & 0xFFFF
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x22, 0x26):
            r = (x >> 21) & 31
            if lui.get(r) == 0x1F80:
                s = x & 0xFFFF; s = s - 65536 if s & 0x8000 else s
                reg = 0x1F800000 + s
                if reg >= 0x1F801000: hw[reg & ~7].add(f)
        elif op == 9 and ((x >> 16) & 31) == 10 and (x & 0xFFFF) in (0xA0, 0xB0, 0xC0): ab = x & 0xFFFF
        elif op == 9 and ((x >> 16) & 31) == 9 and ab: bios[(ab, x & 0xFFFF)].add(f); ab = None
        elif op == 0x12: cop2.add(f)
print("%d of %d functions were never entered" % (len(unreached), len(fs)))
print("\nhardware register groups addressed by never-entered functions, and whether the runs touched them at all:")
for r, fl in sorted(hw.items()):
    seen = ((r - 0x1F801000) >> 3) in io
    print("  %08x %-14s %3d functions   %s" % (r, name(r), len(fl), "touched by the runs" if seen else "NEVER touched by any run"))
print("\nBIOS stubs called by never-entered functions: %s" % ", ".join("%X(%02X)x%d" % (t, n, len(v)) for (t, n), v in sorted(bios.items())))
print("never-entered functions using the GTE (COP2): %d" % len(cop2))

if "--stubs" in a:
    for (t, n), v in sorted(bios.items()): print("  %X(%02X) in function(s) %s" % (t, n, " ".join("%08x" % x for x in sorted(v))))
