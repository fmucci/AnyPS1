#!/usr/bin/env python3
"""Acceptance check 4 of docs 18: every BIOS call the executables import is implemented by the runtime or listed as a stub.
Finds the BIOS call stubs of each executable (`li t2,A0h/B0h/C0h ; jr t2 ; li t1,N`), then looks for N in the runtime's dispatch code.
usage: python3 -I tools/bios_imports_check.py [EXE ...]      (default: the executables of game/game.json)
Exit 1 when an imported call has neither an implementation nor a row in docs/stubs.md (write `A(A1h)` / `B(38h)` style names there for calls that are intentionally fatal (the runtime would stop with `BIOS X(NN) is not implemented` when it is called; whether it is called at all needs the coverage report)."""
import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, ROOT)
from tools.recomp import gamecfg  # noqa: E402

names = {}
try:
    for line in open(os.path.join(ROOT, "docs", "bios-names.csv")):
        t, n, nm = line.strip().split(",", 2)
        names[(t, int(n, 16))] = nm
except OSError:
    pass
impl = set()
for f in ("runtime/hle.c", "runtime/biosc.inc", "runtime/card.c"):
    src = open(os.path.join(ROOT, f)).read()
    for t, n in re.findall(r"t == '([ABC])' && \(?n == 0x([0-9A-Fa-f]{2})", src):
        impl.add((t, int(n, 16)))
    for t, a, b in re.findall(r"t == '([ABC])' && n >= 0x([0-9A-Fa-f]{2}) && n <= 0x([0-9A-Fa-f]{2})", src):
        impl |= {(t, k) for k in range(int(a, 16), int(b, 16) + 1)}
    for t, lst in re.findall(r"t == '([ABC])' && \(((?:n == 0x[0-9A-Fa-f]{2}(?: \|\| )?)+)\)", src):
        impl |= {(t, int(x, 16)) for x in re.findall(r"0x([0-9A-Fa-f]{2})", lst)}
    for m in re.finditer(r"case 0x([0-9A-Fa-f]{2}):", src):
        pass
# biosc.inc dispatches by `if (t == 'A') switch (n) { case 0xNN: ...`
src = open(os.path.join(ROOT, "runtime", "biosc.inc")).read()
parts = re.split(r"if \(t == '([ABC])'\) switch \(n\) \{", src)
for k in range(1, len(parts), 2):
    impl |= {(parts[k], int(x, 16)) for x in re.findall(r"case 0x([0-9A-Fa-f]{2}):", parts[k + 1].split("default: return 0;")[0])}
exes = sys.argv[1:] or [os.path.join(gamecfg.DISC_DIR, e) for e, _p in gamecfg.EXES]
bad = 0
for path in exes:
    d = open(path, "rb").read()
    base, size = struct.unpack("<II", d[0x18:0x20])[0], struct.unpack("<I", d[0x1C:0x20])[0]
    base = struct.unpack("<I", d[0x18:0x1C])[0]
    words = struct.unpack("<%dI" % (size // 4), d[0x800:0x800 + size // 4 * 4])
    found = {}
    for i in range(len(words) - 2):
        w = words[i]
        if (w >> 16) == 0x240A and (w & 0xFFFF) in (0xA0, 0xB0, 0xC0) and words[i + 1] == 0x01400008 and (words[i + 2] >> 16) == 0x2409:
            found[("ABC"[((w & 0xFFFF) - 0xA0) >> 4], words[i + 2] & 0xFF)] = base + 4 * i
    miss = sorted(k for k in found if k not in impl)
    reg = open(os.path.join(ROOT, "docs", "stubs.md")).read().lower() if os.path.exists(os.path.join(ROOT, "docs", "stubs.md")) else ""
    unregistered = [k for k in miss if ("%s(%02xh)" % k).lower() not in reg and ("%s(%02x)" % k).lower() not in reg]
    print("%s: %d BIOS call stubs; %d without an implementation%s; %d of those not registered in docs/stubs.md%s" % (os.path.basename(path), len(found), len(miss), " (" + ", ".join("%s(%02X)" % k for k in miss) + ")" if miss else "", len(unregistered), ": " + ", ".join("%s(%02X)" % k for k in unregistered) if unregistered else ""))
    bad += len(unregistered)
sys.exit(1 if bad else 0)
