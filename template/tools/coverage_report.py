#!/usr/bin/env python3
"""Merge coverage files written by runs of `build/ps1_game` (env PS1_COVERAGE=file; build with --coverage for function-level data) and print what was and was NOT exercised.
usage: python3 -I tools/coverage_report.py RUN1.json [RUN2.json ...] [--md OUT.md] [--funcs build/game/gen_main.funcs]
The report is the replacement for "I played it": it says which device features, BIOS calls, interrupt sources and recompiled functions no run touched, so unknowns are stated, not hidden.
Exit code is 0 always; a threshold check is the caller's job (docs 18: coverage numbers go in the README progress block)."""
import glob
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
md_out = sys.argv[sys.argv.index("--md") + 1] if "--md" in sys.argv else None
funcs_file = sys.argv[sys.argv.index("--funcs") + 1] if "--funcs" in sys.argv else None
for opt in ("--md", "--funcs"):
    if opt in sys.argv:
        args = [a for a in args if a != sys.argv[sys.argv.index(opt) + 1]]
runs = [json.load(open(p)) for p in args]
if not runs:
    sys.exit(__doc__)

merged = {}
fn = set()
for r in runs:
    for g, d in r.items():
        if g == "functions":
            fn |= set(d)
        else:
            for k, v in d.items():
                merged.setdefault(g, {}).setdefault(int(k), 0)
                merged[g][int(k)] += v

GP0 = {"polygon (20h-3Fh)": range(0x20, 0x40), "line (40h-5Fh)": range(0x40, 0x60), "rectangle/sprite (60h-7Fh)": range(0x60, 0x80), "VRAM->VRAM copy (80h-9Fh)": range(0x80, 0xA0), "CPU->VRAM (A0h-BFh)": range(0xA0, 0xC0), "VRAM->CPU (C0h-DFh)": range(0xC0, 0xE0), "fill (02h)": [0x02]}
GP0_SUB = {"textured polygon": [o for o in range(0x24, 0x40) if o & 4], "gouraud polygon": [o for o in range(0x30, 0x40)], "semi-transparent (bit1)": [o for o in range(0x20, 0x80) if o & 2], "poly-line (48h-5Fh)": [o for o in range(0x48, 0x60) if o & 8]}
GP1 = {0: "reset", 1: "reset command buffer", 2: "ack irq", 3: "display enable", 4: "DMA direction", 5: "display start", 6: "h range", 7: "v range", 8: "display mode", 0x10: "get info"}
CD = {1: "GetStat", 2: "Setloc", 3: "Play", 6: "ReadN", 7: "MotorOn", 8: "Stop", 9: "Pause", 0xA: "Init", 0xB: "Mute", 0xC: "Demute", 0xD: "Setfilter", 0xE: "Setmode", 0x10: "GetlocL", 0x11: "GetlocP", 0x13: "GetTN", 0x14: "GetTD", 0x15: "SeekL", 0x19: "Test", 0x1A: "GetID", 0x1B: "ReadS"}
IRQ = {0: "VBLANK", 2: "CD-ROM", 3: "DMA", 4: "timer 0", 5: "timer 1", 6: "timer 2", 7: "controller/card"}
MDEC = {1: "decode macroblocks", 2: "set quant tables", 3: "set scale table"}
DMA = {0: "MDEC in", 1: "MDEC out", 2: "GPU", 3: "CD-ROM", 4: "SPU", 6: "OTC"}
SECT = {"video (STR) sectors": 1, "audio (XA) sectors": 2, "data sectors": 4}


def hit(group, k):
    return merged.get(group, {}).get(k, 0)


L = ["# Coverage report", "", "Runs merged: %d. This lists what the runs exercised and, more importantly, what they did NOT." % len(runs), ""]


def section(title, rows, universe_note=""):
    L.append("## " + title)
    if universe_note:
        L.append(universe_note)
    done = [(n, c) for n, c in rows if c]
    miss = [n for n, c in rows if not c]
    L.append("exercised %d of %d" % (len(done), len(rows)))
    if done:
        L.append("- exercised: " + ", ".join("%s (%d)" % (n, c) for n, c in done))
    if miss:
        L.append("- **not exercised**: " + ", ".join(miss))
    L.append("")


section("GPU drawing commands (GP0)", [(n, sum(hit("gp0", o) for o in rng)) for n, rng in GP0.items()])
section("GPU drawing sub-features", [(n, sum(hit("gp0", o) for o in rng)) for n, rng in GP0_SUB.items()], "(counts are of commands whose opcode has the feature)")
section("GPU control commands (GP1)", [("%s (%02Xh)" % (n, k), hit("gp1", k)) for k, n in GP1.items()])
section("CD-ROM commands", [("%s (%02Xh)" % (n, k), hit("cd_cmd", k)) for k, n in CD.items()])
section("CD-ROM sector kinds", [(n, sum(v for k, v in merged.get("cd_sector", {}).items() if k & t)) for n, t in SECT.items()])
section("DMA channels", [("%s (%d)" % (n, k), hit("dma_chan", k)) for k, n in DMA.items()])
section("MDEC commands", [("%s (%d)" % (n, k), hit("mdec_cmd", k)) for k, n in MDEC.items()])
section("SPU voices keyed on", [("voice %d" % v, hit("spu_keyon_voice", v)) for v in range(24)])
section("Interrupt sources delivered", [("%s (%d)" % (n, k), hit("irq", k)) for k, n in IRQ.items()])
section("Other", [("guest exceptions (syscall/break)", sum(merged.get("exception", {}).values())), ("interpreter-tier entries (code not translated ahead of time)", sum(merged.get("interp_entry", {}).values()))])
bios = sorted(merged.get("bios", {}).items())
L.append("## BIOS calls made")
L.append(", ".join("%s(%02X)x%d" % ("ABC"[k // 256], k % 256, v) for k, v in bios) or "none")
L.append("")
impl = set()
try:
    src = open(os.path.join(ROOT, "runtime", "hle.c")).read() + open(os.path.join(ROOT, "runtime", "biosc.inc")).read() + open(os.path.join(ROOT, "runtime", "card.c")).read()
    for t, n in re.findall(r"t == '([ABC])' && \(?n == 0x([0-9A-Fa-f]{2})", src):
        impl.add("ABC".index(t) * 256 + int(n, 16))
except OSError:
    pass
un = sorted(impl - set(merged.get("bios", {})))
if un:
    L.append("BIOS calls implemented but not exercised: " + ", ".join("%s(%02X)" % ("ABC"[k // 256], k % 256) for k in un))
    L.append("")
regs_r = len(merged.get("io_read", {})); regs_w = len(merged.get("io_write", {}))
L.append("## I/O registers touched\n%d distinct 8-byte register groups read, %d written (0x1F801000-0x1F802FFF)\n" % (regs_r, regs_w))
if funcs_file and os.path.exists(funcs_file):
    allf = {int(x, 16) for x in open(funcs_file).read().split()}
    got = len(allf & fn)
    L.append("## Recompiled functions entered\n%d of %d (%.1f %%)%s\n" % (got, len(allf), 100.0 * got / max(1, len(allf)), "" if fn else " -- build with `build_game.py --coverage` to record function entries"))
elif fn:
    L.append("## Recompiled functions entered\n%d distinct functions (pass --funcs build/game/gen_<prefix>.funcs for the percentage)\n" % len(fn))
text = "\n".join(L)
print(text)
if md_out:
    open(md_out, "w").write(text + "\n")
