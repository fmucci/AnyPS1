#!/usr/bin/env python3
"""Differential test of the interpreter tier (runtime/interp.c) against the Python spec (tools/recomp/spec.py), on the same random fragments as tests/recomp/run_diff.py
(ALU, shifts, mult/div, loads/stores incl. lwl/lwr/swl/swr, COP0/COP2 moves, branches with delay slots, links, exceptions) with random initial states.
usage: python3 -I tests/recomp/run_diff_interp.py [--seed N] [--cases N] [--batch N]"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)
import run_diff as D  # noqa: E402
from tools.recomp import emit, hostenv  # noqa: E402

STUBS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "rt.h"
void test_mem_set(const uint8_t *);
const uint8_t *test_mem_get(void);
static const uint32_t *g_code; static unsigned g_len;
uint32_t rt_interp_test_fetch(uint32_t pc) { return g_code[(pc - 0x1000u) / 4]; }
int rt_interp_test_out(uint32_t pc) { return pc < 0x1000u || pc >= 0x1000u + 4 * g_len || (pc & 3); }
const char *rt_fn_name(uint32_t a) { (void)a; return NULL; }
void rt_call(Cpu *c, uint32_t t) { (void)c; fprintf(stderr, "unexpected rt_call %08x\n", t); abort(); }
void rt_yield(Cpu *c) { c->yield = RT_YIELD_PERIOD; }
void rt_fatal(const Cpu *c, const char *f, ...) { (void)c; fprintf(stderr, "rt_fatal: %s\n", f); abort(); }
uint8_t *rt_ram_ptr(Cpu *c) { (void)c; return NULL; }
'''


def build(cases):
    out = [STUBS]
    n = len(cases)
    maxlen = max(len(w) for w, _ in cases)
    out.append("static const unsigned LEN[%d] = {%s};" % (n, ",".join(str(len(w)) for w, _ in cases)))
    out.append("static const uint32_t CODE[%d][%d] = {%s};" % (n, maxlen, ",".join(D.c_array(list(w) + [0] * (maxlen - len(w))) for w, _ in cases)))
    out.append("static const uint32_t IR[%d][32] = {%s};" % (n, ",".join(D.c_array(st.r) for _, st in cases)))
    out.append("static const uint32_t IHL[%d][2] = {%s};" % (n, ",".join(D.c_array([st.hi, st.lo]) for _, st in cases)))
    out.append("static const uint32_t I0[%d][32] = {%s};" % (n, ",".join(D.c_array(st.cop0) for _, st in cases)))
    out.append("static const uint32_t ID[%d][32] = {%s};" % (n, ",".join(D.c_array(st.cop2d) for _, st in cases)))
    out.append("static const uint32_t IC[%d][32] = {%s};" % (n, ",".join(D.c_array(st.cop2c) for _, st in cases)))
    out.append("static const uint8_t IM[%d][256] = {%s};" % (n, ",".join("{" + ",".join(str(b) for b in st.mem) + "}" for _, st in cases)))
    out.append(r'''
int main(void) {
    for (unsigned i = 0; i < %d; i++) {
        Cpu c; memset(&c, 0, sizeof c);
        memcpy(c.r, IR[i], sizeof c.r); c.hi = IHL[i][0]; c.lo = IHL[i][1];
        memcpy(c.cop0, I0[i], sizeof c.cop0); memcpy(c.gte_d, ID[i], sizeof c.gte_d); memcpy(c.gte_c, IC[i], sizeof c.gte_c);
        c.yield = RT_YIELD_PERIOD;
        test_mem_set(IM[i]);
        g_code = CODE[i]; g_len = LEN[i];
        c.r[31] = IR[i][31];
        rt_interp_call(&c, 0x1000u);
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


def run_batch(cases):
    src = build(cases)
    with tempfile.TemporaryDirectory() as td:
        cf, exe = os.path.join(td, "t.c"), os.path.join(td, "t")
        open(cf, "w").write(src)
        p = subprocess.run([hostenv.CC, "-std=gnu11", "-O1", "-Wall", "-Wno-unused-label", "-DRT_INTERP_TEST", "-I", os.path.join(ROOT, "runtime"), cf,
                            os.path.join(ROOT, "runtime", "interp.c"), os.path.join(HERE, "harness_support.c"), "-o", exe], capture_output=True, text=True)
        if p.returncode:
            open("/tmp/ps1_failed_interp_test.c", "w").write(src)
            raise SystemExit("C compile failed (source kept in /tmp/ps1_failed_interp_test.c):\n" + p.stderr[:3000])
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("test program failed: %s" % r.stderr[:500])
    return r.stdout.strip().split("\n")


def main():
    a = sys.argv[1:]
    get = lambda k, d: type(d)(a[a.index(k) + 1]) if k in a else d  # noqa: E731
    seed, ncases, batch = get("--seed", 1), get("--cases", 3000), get("--batch", 300)
    rng = random.Random(seed)
    stats = {"compared": 0, "mismatch": 0, "skipped": 0}
    pending, fails = [], []

    def flush():
        nonlocal pending
        if not pending:
            return
        lines = run_batch(pending)
        for (words, st), line in zip(pending, lines):
            errs = D.compare(words, st, line)
            stats["compared"] += 1
            if errs:
                stats["mismatch"] += 1
                fails.append((words, errs))
        pending = []

    produced = 0
    while produced < ncases:
        words = D.gen_fragment(rng)
        st = D.gen_state(rng)
        try:
            D.spec_result(words, st)
        except RuntimeError:
            stats["skipped"] += 1
            produced += 1
            continue
        pending.append((words, st))
        produced += 1
        if len(pending) >= batch:
            flush()
    flush()
    print("stats:", stats)
    for words, errs in fails[:5]:
        print("MISMATCH:", [("%08x" % w) for w in words], errs[:4])
    sys.exit(1 if stats["mismatch"] else 0)


if __name__ == "__main__":
    main()
