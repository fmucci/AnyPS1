#!/usr/bin/env python3
"""Differential test: C emitted by tools/recomp/emit.py (compiled with the host C compiler) vs the Python spec (tools/recomp/spec.py).

usage: python3 -I tests/recomp/run_diff.py [--seed N] [--cases N] [--batch N] [--cc clang]

Generates random instruction fragments (ALU, shifts, mult/div, loads/stores incl. lwl/lwr/swl/swr, COP0/COP2 moves, branches with
delay slots, jal/jalr links, exceptions) with random initial machine states, compiles all fragments into one C program per batch,
runs it, and compares the complete final state (GPRs, HI/LO, memory window, COP0/COP2 arrays, GTE command log, exit PC, exception).
Fragments the emitter rejects on purpose (HazardError / Unsupported) are counted, not compared. Exit code 1 on any mismatch.
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)
import asm as A  # noqa: E402
from tools.recomp import emit, hostenv, spec  # noqa: E402
from tools.recomp.isa import decode  # noqa: E402

BASE = 0x1000
OUT_ADDR = 0x9000
G = list(range(1, 10))          # general registers the generator reads and writes
SMALL = [1, 2, 3]               # small pool to provoke load-delay hazards
PTR = [10, 11, 12]              # pointer registers (never written)
PTR_VAL = {10: spec.MEM_BASE + 0x40, 11: spec.MEM_BASE + 0x80, 12: spec.MEM_BASE + 0xC0}
SPECIAL = [0, 1, 2, 3, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x0000FFFF, 0xFFFF0000, 31, 32, 33]


def rreg(rng, allow0=True, pool=None):
    pool = pool or (SMALL if rng.random() < 0.5 else G)
    return 0 if allow0 and rng.random() < 0.08 else rng.choice(pool)


def alu(rng):
    k = rng.choice(["sll", "srl", "sra", "sllv", "srlv", "srav", "addu", "subu", "and", "or", "xor", "nor", "slt", "sltu",
                    "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui", "mfhi", "mflo", "mthi", "mtlo", "mult", "multu",
                    "div", "divu", "add", "sub", "addi"])
    d, s, t = rreg(rng), rreg(rng), rreg(rng)
    if k in ("sll", "srl", "sra"): return A.r(k, 0, t, d, rng.randrange(32))
    if k in ("sllv", "srlv", "srav", "addu", "subu", "and", "or", "xor", "nor", "slt", "sltu", "add", "sub"): return A.r(k, s, t, d)
    if k in ("mfhi", "mflo"): return A.r(k, 0, 0, d)
    if k in ("mthi", "mtlo"): return A.r(k, s)
    if k in ("mult", "multu", "div", "divu"): return A.r(k, s, t)
    imm = rng.choice([0, 1, 0x7FFF, 0x8000, 0xFFFF, rng.randrange(65536)])
    return A.i(k, s, d, imm)


def memop(rng, kinds):
    k = rng.choice(kinds)
    p = rng.choice(PTR)
    t = rreg(rng)
    if k in ("lw", "sw", "lwc2", "swc2"): off = rng.randrange(-12, 12) * 4
    elif k in ("lh", "lhu", "sh"): off = rng.randrange(-24, 24) * 2
    else: off = rng.randrange(-40, 40)
    if k in ("lwc2", "swc2"): t = rng.randrange(32)
    return A.i(k, p, t, off)


def copmove(rng):
    k = rng.choice(["mfc0", "mtc0", "mfc2", "cfc2", "mtc2", "ctc2", "cop2", "rfe"])
    t = rreg(rng)
    rd = rng.randrange(8, 16) if k in ("mfc0", "mtc0") else rng.randrange(32)
    if k == "mfc0": return A.mfc0(t, rd)
    if k == "mtc0": return A.mtc0(t, rd)
    if k == "mfc2": return A.mfc2(t, rd)
    if k == "cfc2": return A.cfc2(t, rd)
    if k == "mtc2": return A.mtc2(t, rd)
    if k == "ctc2": return A.ctc2(t, rd)
    if k == "rfe": return A.rfe()
    return A.cop2cmd(rng.randrange(1 << 25))


def safe_slot(rng):
    c = rng.random()
    if c < 0.03:
        return rng.choice([A.r("syscall"), A.brk(rng.randrange(1 << 20))])
    if c < 0.45:
        return alu(rng)  # includes add/sub/addi: exception-capable instructions occur in the game's delay slots
    if c < 0.7: return memop(rng, ["lw", "lh", "lhu", "lb", "lbu", "lwl", "lwr"])
    if c < 0.8: return memop(rng, ["sw", "sh", "sb", "swl", "swr"])
    if c < 0.9: return copmove(rng)
    return memop(rng, ["lwc2", "swc2"])


def gen_fragment(rng):
    """Returns words. Branch targets are patched after layout (forward only; never into a slot)."""
    words, groups, slots = [], [], set()
    for _ in range(rng.randint(1, 7)):
        c = rng.random()
        if c < 0.40: words.append(alu(rng))
        elif c < 0.62: words.append(memop(rng, ["lw", "lh", "lhu", "lb", "lbu", "lwl", "lwr"]))
        elif c < 0.72: words.append(memop(rng, ["sw", "sh", "sb", "swl", "swr"]))
        elif c < 0.77: words.append(memop(rng, ["lwc2", "swc2"]))
        elif c < 0.84: words.append(copmove(rng))
        elif c < 0.86: words.append(rng.choice([A.r("syscall"), A.brk(rng.randrange(1 << 20))]))
        else:
            kind = rng.choice(["beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal", "j", "jal", "jr", "jalr", "bc2f", "bc2t"])
            groups.append((len(words), kind))
            words.append(0)
            slots.add(len(words))
            words.append(safe_slot(rng))
    n = len(words)
    for pos, kind in groups:
        cands = list(range(pos + 2, n + 1))  # targets may land on another branch's delay slot (legal; handled by stubs)
        slot_cands = [q for q in cands if q in slots]
        tgt_idx = rng.choice(slot_cands) if slot_cands and rng.random() < 0.4 else rng.choice(cands)  # bias towards delay-slot targets
        outside = rng.random() < 0.25 or tgt_idx == n and rng.random() < 0.5
        tgt = OUT_ADDR + 4 * rng.randrange(8) if outside else BASE + 4 * tgt_idx
        s, t = rreg(rng), rreg(rng)
        rel = (tgt - (BASE + 4 * pos + 4)) // 4
        if kind in ("beq", "bne"): w = A.i(kind, s, t, rel)
        elif kind in ("blez", "bgtz"): w = A.i(kind, s, 0, rel)
        elif kind in ("bltz", "bgez", "bltzal", "bgezal"): w = A.regimm(kind, s, rel)
        elif kind in ("j", "jal"): w = A.j(kind, tgt)
        elif kind == "jr": w = A.r("jr", 13)
        elif kind == "jalr": w = A.r("jalr", 13, 0, rng.choice(G + [31]))
        else: w = A.bc2(kind == "bc2t", rel)
        words[pos] = w
    return words


# Directed regressions (always run). Fragments that once exposed an emitter bug:
# a load in a delay slot (mfc0 r3) whose taken successor overwrites r3 with another load, followed by a reader of r3: committing the slot load
# early is observable, so the emitter must REJECT it (HazardError); the variant without the reader must be accepted and match the spec.
# (second fragment: a chain of two loads into r1 after the slot load, then a reader of r1)
DIRECTED_REJECT = [[0x48C11000, 0x49000001, 0x40036000, 0x8D63FFDC, 0x00231821, 0x2C41FFFF],
                   [0x04402001, 0x8D41FFE8, 0x8181FFE0, 0x81610000, 0x00244824, 0x48470800]]
DIRECTED_OK = [[0x48C11000, 0x49000001, 0x40036000, 0x8D63FFDC, 0x00000000, 0x2C41FFFF]]


def gen_state(rng):
    st = spec.State()
    for k in range(1, 32):
        st.r[k] = rng.choice(SPECIAL) if rng.random() < 0.5 else rng.getrandbits(32)
    for p, v in PTR_VAL.items():
        st.r[p] = v
    st.r[13] = OUT_ADDR
    st.hi, st.lo = rng.getrandbits(32), rng.getrandbits(32)
    st.mem = bytearray(rng.getrandbits(8) for _ in range(spec.MEM_SIZE))
    for k in range(32):
        st.cop0[k], st.cop2d[k], st.cop2c[k] = rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32)
    return st


def c_array(vals, per=8):
    return "{" + ",".join("0x%08Xu" % v for v in vals) + "}"


def build_c(cases):
    out = ['#include <stdio.h>', '#include <string.h>', '#include "cpu.h"',
           'void test_mem_set(const uint8_t *);', 'const uint8_t *test_mem_get(void);']
    names = []
    for idx, (words, st) in enumerate(cases):
        src, _ = emit.emit_fragment(words, BASE, "frag_%d" % idx)
        out.append(src)
        names.append("frag_%d" % idx)
    out.append("typedef void (*fn)(Cpu *);")
    out.append("static fn fns[] = {%s};" % ",".join(names))
    n = len(cases)
    out.append("static const uint32_t IR[%d][32] = {%s};" % (n, ",".join(c_array(st.r) for _, st in cases)))
    out.append("static const uint32_t IHL[%d][2] = {%s};" % (n, ",".join(c_array([st.hi, st.lo]) for _, st in cases)))
    out.append("static const uint32_t I0[%d][32] = {%s};" % (n, ",".join(c_array(st.cop0) for _, st in cases)))
    out.append("static const uint32_t ID[%d][32] = {%s};" % (n, ",".join(c_array(st.cop2d) for _, st in cases)))
    out.append("static const uint32_t IC[%d][32] = {%s};" % (n, ",".join(c_array(st.cop2c) for _, st in cases)))
    out.append("static const uint8_t IM[%d][256] = {%s};" % (n, ",".join("{" + ",".join(str(b) for b in st.mem) + "}" for _, st in cases)))
    out.append(r'''
int main(void) {
    for (unsigned i = 0; i < %d; i++) {
        Cpu c; memset(&c, 0, sizeof c);
        memcpy(c.r, IR[i], sizeof c.r); c.hi = IHL[i][0]; c.lo = IHL[i][1];
        memcpy(c.cop0, I0[i], sizeof c.cop0); memcpy(c.gte_d, ID[i], sizeof c.gte_d); memcpy(c.gte_c, IC[i], sizeof c.gte_c);
        test_mem_set(IM[i]);
        fns[i](&c);
        printf("%%u %%x %%x %%x %%x %%x %%x %%x %%x", i, c.exc, c.exc_pc, c.exc_arg, c.pc, c.hi, c.lo, c.gte_ncmds, c.gte_last_cmd);
        for (int k = 0; k < 32; k++) printf(" %%x", c.r[k]);
        for (int k = 0; k < 32; k++) printf(" %%x", c.cop0[k]);
        for (int k = 0; k < 32; k++) printf(" %%x", c.gte_d[k]);
        for (int k = 0; k < 32; k++) printf(" %%x", c.gte_c[k]);
        const uint8_t *m = test_mem_get();
        printf(" ");
        for (int k = 0; k < 256; k++) printf("%%02x", m[k]);
        printf("\n");
    }
    return 0;
}
''' % n)
    return "\n".join(out)


def spec_result(words, st):
    st = _clone(st)
    pc = spec.run_fragment(st, words, BASE)
    return st, pc


def _clone(st):
    n = spec.State()
    n.r, n.hi, n.lo = list(st.r), st.hi, st.lo
    n.mem, n.cop0, n.cop2d, n.cop2c = bytearray(st.mem), list(st.cop0), list(st.cop2d), list(st.cop2c)
    return n


def compare(words, st, line):
    f = line.split()
    exc, exc_pc, exc_arg, pc, hi, lo, ncmd, lastcmd = [int(x, 16) for x in f[1:9]]
    r = [int(x, 16) for x in f[9:41]]
    c0 = [int(x, 16) for x in f[41:73]]
    d = [int(x, 16) for x in f[73:105]]
    cc = [int(x, 16) for x in f[105:137]]
    mem = bytes.fromhex(f[137])
    sst, spc = spec_result(words, st)
    if sst.exc is not None:
        got = (exc, exc_pc, exc_arg)
        return [] if got == tuple(sst.exc) else ["exception C=%r spec=%r" % (got, sst.exc)]
    errs = []
    if exc: errs.append("C raised exception %r, spec none" % ((exc, exc_pc, exc_arg),))
    if pc != spc: errs.append("exit pc C=%#x spec=%#x" % (pc, spc))
    for k in range(32):
        if r[k] != sst.r[k]: errs.append("r%d C=%#x spec=%#x" % (k, r[k], sst.r[k]))
    if (hi, lo) != (sst.hi, sst.lo): errs.append("hi/lo C=%#x/%#x spec=%#x/%#x" % (hi, lo, sst.hi, sst.lo))
    if mem != bytes(sst.mem): errs.append("memory differs")
    if c0 != sst.cop0: errs.append("cop0 differs")
    if d != sst.cop2d: errs.append("cop2 data differs")
    if cc != sst.cop2c: errs.append("cop2 control differs")
    if (ncmd, lastcmd) != (len(sst.cop2cmds), sst.cop2cmds[-1] if sst.cop2cmds else 0): errs.append("cop2 command log differs")
    return errs


def run_batch(cases, cc):
    src = build_c(cases)
    with tempfile.TemporaryDirectory() as td:
        cf, exe = os.path.join(td, "t.c"), os.path.join(td, "t")
        open(cf, "w").write(src)
        p = subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wno-unused-label", "-Wno-unused-variable", "-I", os.path.join(ROOT, "runtime"), cf,
                            os.path.join(HERE, "harness_support.c"), "-o", exe], capture_output=True, text=True)
        if p.returncode:
            open("/tmp/ps1_failed_test.c", "w").write(src)
            raise SystemExit("C compile failed (source kept in /tmp/ps1_failed_test.c):\n" + p.stderr[:3000])
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("test program failed: %s" % r.stderr[:500])
    return r.stdout.strip().split("\n")


def main():
    a = sys.argv[1:]
    get = lambda k, d: type(d)(a[a.index(k) + 1]) if k in a else d
    seed, ncases, batch, cc = get("--seed", 1), get("--cases", 3000), get("--batch", 300), get("--cc", hostenv.CC)
    rng = random.Random(seed)
    stats = {"compared": 0, "rejected_hazard": 0, "rejected_unsupported": 0, "mismatch": 0, "exceptions": 0, "unchecked_slot_loads": 0, "with_slot_target_stub": 0}
    names = {}
    pending = []
    fails = []

    def flush():
        nonlocal pending
        if not pending:
            return
        lines = run_batch(pending, cc)
        assert len(lines) == len(pending), (len(lines), len(pending))
        for (words, st), line in zip(pending, lines):
            errs = compare(words, st, line)
            stats["compared"] += 1
            if spec_result(words, st)[0].exc is not None:
                stats["exceptions"] += 1
            if errs:
                stats["mismatch"] += 1
                fails.append((words, errs))
        pending = []

    for words in DIRECTED_REJECT:
        try:
            emit.emit_fragment(words, BASE, "x")
            raise SystemExit("directed regression: the emitter accepted a fragment it must reject: %r" % words)
        except emit.HazardError:
            pass
    for words in DIRECTED_OK:
        for _ in range(25):
            pending.append((words, gen_state(rng)))
    produced = 0
    while produced < ncases:
        words = gen_fragment(rng)
        try:
            src_x, rep = emit.emit_fragment(words, BASE, "x")
            stats["with_slot_target_stub"] += 1 if "S_" in src_x else 0
        except emit.HazardError:
            stats["rejected_hazard"] += 1
            produced += 1
            continue
        except emit.Unsupported:
            stats["rejected_unsupported"] += 1
            produced += 1
            continue
        stats["unchecked_slot_loads"] += len(rep["unchecked_slot_loads"])
        for w in words:
            names[decode(w).name] = names.get(decode(w).name, 0) + 1
        st = gen_state(rng)
        try:
            spec_result(words, st)
        except RuntimeError:
            continue
        pending.append((words, st))
        produced += 1
        if len(pending) >= batch:
            flush()
    flush()
    print("stats:", stats)
    print("instruction forms exercised (%d): %s" % (len(names), " ".join("%s=%d" % kv for kv in sorted(names.items()))))
    for words, errs in fails[:5]:
        print("MISMATCH:", [("%08x" % w) for w in words], [repr(decode(w, BASE + 4 * k)) for k, w in enumerate(words)], errs[:4])
    sys.exit(1 if stats["mismatch"] else 0)


if __name__ == "__main__":
    main()
