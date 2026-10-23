#!/usr/bin/env python3
"""Mutation check for the differential test: each mutation deliberately breaks one rule in a COPY of tools/recomp/emit.py; the
differential test (tests/recomp/run_diff.py) must then report mismatches. A surviving mutation means the tests cannot see that rule.
usage: python3 -I tests/recomp/mutation_check.py [--cases N]"""
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)
import run_diff  # noqa: E402

SRC = open(os.path.join(ROOT, "tools", "recomp", "emit.py")).read()
MUTATIONS = {
    "lwl merge mask off by one byte": ("0x00FFFFFFu >> (8u * (a & 3u))", "0x00FFFFFFu >> (8u * ((a + 1u) & 3u))"),
    "lwr uses wrong shift": ("(w >> (8u * (a & 3u)))", "(w >> (8u * ((a + 1u) & 3u)))"),
    "no lwl/lwr forwarding of the pending load": ("base = pend[1] if pend and pend[0] == rt else B", "base = B"),
    "pending load is committed even if the next instruction writes the same register": ("if written != pend[0]:", "if True:"),
    "jal links pc+4": ("link = (31, U(b.pc + 8))", "link = (31, U(b.pc + 4))"),
    "swr mask wrong": ("(m & ~(0xFFFFFFFFu << sh)) | (v << sh)", "(m & ~(0x7FFFFFFFu << sh)) | (v << sh)"),
    "swl mask wrong": ("(m & ~(0xFFFFFFFFu >> sh)) | (v >> sh)", "(m & ~(0x7FFFFFFFu >> sh)) | (v >> sh)"),
    "load in a delay slot is never committed": ("            lines.append(W(d, t))\n        # transfer", "            pass\n        # transfer"),
    "overflow check ignores one operand": ("((a ^ r) & (b ^ r)) >> 31) { rt_exception(c, %s, 12u, 0u); return; } %s }\" % (A, B,", "((a ^ r) & (a ^ b)) >> 31) { rt_exception(c, %s, 12u, 0u); return; } %s }\" % (A, B,"),
    "slti compares unsigned": ("s.append(W(rt, \"(uint32_t)((int32_t)%s < (int32_t)%s)\" % (A, U(ins.simm))))", "s.append(W(rt, \"(uint32_t)(%s < %s)\" % (A, U(ins.simm))))"),
    "branch condition read after the link write": ("lines.append(\"int br = (%s);\" % cond)", "lines.append(\"int br = 1;\")"),
    "slot-target stub continues at the wrong instruction": ("lines.append(\"goto L_%d;\" % (sidx + 1))", "lines.append(\"goto L_%d;\" % (sidx + 2 if sidx + 2 < n else sidx + 1))"),
    "mult uses unsigned operands": ("\"{ int64_t p = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s;", "\"{ int64_t p = (int64_t)(uint32_t)%s * (int64_t)(uint32_t)%s;"),
}


def main():
    cases = int(sys.argv[sys.argv.index("--cases") + 1]) if "--cases" in sys.argv else 2500
    survivors = []
    for name, (old, new) in MUTATIONS.items():
        if old not in SRC:
            print("MUTATION TARGET NOT FOUND:", name)
            survivors.append(name)
            continue
        mod = types.ModuleType("tools.recomp.emit_mut")
        mod.__package__ = "tools.recomp"
        exec(compile(SRC.replace(old, new, 1), "emit_mut", "exec"), mod.__dict__)
        run_diff.emit = mod
        sys.argv = ["run_diff", "--cases", str(cases), "--seed", "7"]
        try:
            run_diff.main()
            killed = False
        except SystemExit as e:
            killed = e.code == 1 or isinstance(e.code, str)  # a string code = the mutated C failed to compile or crashed: also detected
        print("%-80s %s" % (name, "killed" if killed else "SURVIVED"))
        if not killed:
            survivors.append(name)
    print("\nsurvivors:", survivors or "none")
    sys.exit(1 if survivors else 0)


if __name__ == "__main__":
    main()
