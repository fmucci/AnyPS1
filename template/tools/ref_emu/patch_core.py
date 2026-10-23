#!/usr/bin/env python3
"""Adds our own debug instrumentation to a LOCAL clone of the reference emulator (PCSX-ReARMed, GPL: the modified clone stays in work/emu/, git-ignored, never committed or redistributed).
usage: python3 -I tools/ref_emu/patch_core.py work/emu/pcsx_rearmed
Idempotent; refuses (exit 1) when an anchor line of the upstream source is not found, so a changed upstream is noticed instead of silently half-patched.
Symbols it defines (all `ps1_*`, written by the host in host.c):  ps1_frame  current frame;  ps1_npc/ps1_pcs  guest PCs to log with a0-a3/ra/sp;  ps1_wlo/ps1_whi  log stores into [lo,hi);
ps1_jlo/ps1_jhi  log each distinct jal/jalr target while the frame is in the range;  ps1_blo/ps1_bhi  log every BIOS A/B call (number, a0-a3, ra) in that frame range."""
import os
import sys

root = sys.argv[1]


def edit(rel, steps):
    p = os.path.join(root, rel)
    s = open(p).read()
    if "ps1_frame" in s and rel != "libpcsxcore/psxinterpreter.c":
        print("already patched:", rel)
        return
    if rel == "libpcsxcore/psxinterpreter.c" and "ps1_poll_check" in s:
        print("already patched:", rel)
        return
    for anchor, new, where in steps:
        if anchor not in s:
            sys.exit("anchor not found in %s: %r (upstream changed?)" % (rel, anchor[:70]))
        i = s.index(anchor)
        s = s[:i] + new + anchor + s[i + len(anchor):] if where == "before" else s[:i + len(anchor)] + new + s[i + len(anchor):]
    open(p, "w").write("#include <stdio.h>\n" + s)
    print("patched:", rel)


INTERP = '''unsigned ps1_npc; unsigned ps1_pcs[32]; unsigned ps1_frame;
extern unsigned ps1_wlo, ps1_whi;
static unsigned ps1_wlast = 0xdeadbeef;
static inline void ps1_poll_check(psxRegisters *regs, u32 pc) {      /* --poll ADDR: report every change of that RAM word with the pc that follows */
	unsigned v = *(unsigned *)(regs->ptrs.psxM + (ps1_wlo & 0x1ffffc));
	if (v != ps1_wlast) { fprintf(stderr, "[chg] frame %u %08x: %08x -> %08x before pc=%08x ra=%08x\\n", ps1_frame, ps1_wlo, ps1_wlast, v, pc, regs->GPR.n.ra); ps1_wlast = v; }
}
unsigned ps1_blo = 1, ps1_bhi = 0;
unsigned ps1_jlo, ps1_jhi;
static unsigned char ps1_jseen[1 << 19];
static inline void ps1_jal_check(psxRegisters *regs, u32 pc) {
	if (ps1_frame < ps1_jlo || ps1_frame > ps1_jhi) return;
	u32 w = *(u32 *)(regs->ptrs.psxM + (pc & 0x1ffffc));
	u32 t = 0;
	if ((w >> 26) == 3) t = (pc & 0xf0000000) | ((w & 0x3ffffff) << 2);
	else if ((w >> 26) == 0 && (w & 0x3f) == 9) t = regs->GPR.r[(w >> 21) & 31];
	else return;
	u32 k = (t >> 2) & 0x7ffff;
	if (!ps1_jseen[k]) { ps1_jseen[k] = 1; fprintf(stderr, "[jal] frame %u %08x\\n", ps1_frame, t); }
}
static inline void ps1_pc_check(psxRegisters *regs, u32 pc) {
	for (unsigned i = 0; i < ps1_npc; i++) if (ps1_pcs[i] == pc)
		fprintf(stderr, "[pc] frame %u cyc %u pc=%08x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x\\n", ps1_frame, regs->cycle, pc, regs->GPR.n.a0, regs->GPR.n.a1, regs->GPR.n.a2, regs->GPR.n.a3, regs->GPR.n.ra, regs->GPR.n.sp);
}
'''
edit("libpcsxcore/psxinterpreter.c", [
    ("static inline void execI_(", INTERP, "before"),
    ("\tu32 pc = regs->pc;\n", "\tif (ps1_npc) ps1_pc_check(regs, pc);\n\tif (ps1_jhi) ps1_jal_check(regs, pc);\n\tif (ps1_whi) ps1_poll_check(regs, pc);\n", "after"),
])
# the poll check above reuses ps1_wlo/ps1_whi, defined with the store watch
edit("libpcsxcore/psxmem.c", [
    ("void psxMemWrite8(psxRegisters *regs, u32 mem, u32 value) {\n", "\tPS1_W(8);\n", "after"),
    ("void psxMemWrite16(psxRegisters *regs, u32 mem, u32 value) {\n", "\tPS1_W(16);\n", "after"),
    ("void psxMemWrite32(psxRegisters *regs, u32 mem, u32 value) {\n", "\tPS1_W(32);\n", "after"),
])
p = os.path.join(root, "libpcsxcore/psxmem.c")
s = open(p).read()
if "#define PS1_W" not in s:
    macro = ('unsigned ps1_wlo, ps1_whi; extern unsigned ps1_frame;\n'
             '#define PS1_W(sz) do { if (ps1_whi && (mem & 0x1fffffff) >= ps1_wlo && (mem & 0x1fffffff) < ps1_whi) fprintf(stderr, "[wr] frame %u pc=%08x %d bytes at %08x = %08x ra=%08x\\n", ps1_frame, regs->pc - 4, sz, mem, value, regs->GPR.n.ra); } while (0)\n')
    s = s.replace("#include <stdio.h>\n", "#include <stdio.h>\n" + macro, 1)
    open(p, "w").write(s)
edit("libpcsxcore/psxbios.c", [
    ("static void hleA0() {\n\tu32 call = t1 & 0xff;\n",
     '\tif (ps1_frame >= ps1_blo && ps1_frame <= ps1_bhi && call != 0x2a && call != 0x2b && call != 0x44) fprintf(stderr, "[bios] frame %u A(%02x) %08x %08x %08x %08x ra=%08x\\n", ps1_frame, call, a0, a1, a2, a3, ra);\n', "after"),
    ("static void hleB0() {\n\tu32 call = t1 & 0xff;\n",
     '\tif (ps1_frame >= ps1_blo && ps1_frame <= ps1_bhi && call != 0x17 && call != 0x0b && call != 0x0c && call != 0x0d) fprintf(stderr, "[bios] frame %u B(%02x) %08x %08x %08x %08x ra=%08x\\n", ps1_frame, call, a0, a1, a2, a3, ra);\n', "after"),
    ("static void hleA0() {", "extern unsigned ps1_frame, ps1_blo, ps1_bhi;\n", "before"),
])
