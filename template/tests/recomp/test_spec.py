"""Known-answer tests for tools/recomp/spec.py. Every expected value is derived BY HAND from psx-spx / the MIPS-I definition,
not by running the spec, so these anchor the reference that the C emitter is later compared against."""
import os
import sys
import unittest

here = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(here, "..", ".."))
sys.path.insert(0, here)
import asm as A  # noqa: E402
from tools.recomp import spec  # noqa: E402

B = 0x1000
W0 = spec.MEM_BASE


def run(words, regs=None, mem=None, cop0=None, cop2c=None):
    st = spec.State()
    for k, v in (regs or {}).items():
        st.r[k] = v
    for off, bs in (mem or {}).items():
        st.mem[off:off + len(bs)] = bytes(bs)
    for k, v in (cop0 or {}).items():
        st.cop0[k] = v
    for k, v in (cop2c or {}).items():
        st.cop2c[k] = v
    pc = spec.run_fragment(st, words, B)
    return st, pc


class T(unittest.TestCase):
    def test_load_delay_old_value(self):
        # lw r1,0(r10); addu r2,r1,r0 (sees OLD r1=5); addu r3,r1,r0 (sees loaded 9)
        st, pc = run([A.i("lw", 10, 1, 0), A.r("addu", 1, 0, 2), A.r("addu", 1, 0, 3)],
                     {1: 5, 10: W0}, {0: [9, 0, 0, 0]})
        self.assertEqual((st.r[1], st.r[2], st.r[3]), (9, 5, 9))
        self.assertEqual(pc, B + 12)

    def test_load_then_write_same_register_loses_load(self):
        # lw r1; addiu r1,r0,7; addu r2,r1,r0  -> r1 = 7 (assumption documented in spec.py), r2 = 7
        st, _ = run([A.i("lw", 10, 1, 0), A.i("addiu", 0, 1, 7), A.r("addu", 1, 0, 2)], {10: W0}, {0: [9, 0, 0, 0]})
        self.assertEqual((st.r[1], st.r[2]), (7, 7))

    def test_load_to_zero_is_ignored(self):
        st, _ = run([A.i("lw", 10, 0, 0), A.r("addu", 0, 0, 2)], {10: W0}, {0: [9, 0, 0, 0]})
        self.assertEqual((st.r[0], st.r[2]), (0, 0))

    def test_lwl_lwr_unaligned_word(self):
        mem = {0: [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]}
        # unaligned word at 0x2001 = bytes 22 33 44 55 -> 0x55443322, both orders, then a reader of r1
        for pair in ([A.i("lwl", 10, 1, 3), A.i("lwr", 10, 1, 0)], [A.i("lwr", 10, 1, 0), A.i("lwl", 10, 1, 3)]):
            st, _ = run(pair + [A.nop(), A.r("addu", 1, 0, 2)], {10: W0 + 1, 1: 0xDEADBEEF}, mem)
            self.assertEqual((st.r[1], st.r[2]), (0x55443322, 0x55443322), pair)

    def test_lwl_lwr_without_forwarding_would_differ(self):
        # if lwr merged with the OLD r1 (0xDEADBEEF) instead of the pending lwl value the top byte would be wrong
        mem = {0: [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]}
        st, _ = run([A.i("lwr", 10, 1, 0), A.i("lwl", 10, 1, 3)], {10: W0 + 1, 1: 0xDEADBEEF}, mem)
        self.assertEqual(st.r[1], 0x55443322)

    def test_swl_swr_store_unaligned(self):
        st, _ = run([A.i("swr", 10, 1, 0), A.i("swl", 10, 1, 3)], {10: W0 + 1, 1: 0xAABBCCDD}, {0: [0] * 8})
        # little-endian bytes of 0xAABBCCDD at address 1..4: DD CC BB AA
        self.assertEqual(list(st.mem[0:8]), [0, 0xDD, 0xCC, 0xBB, 0xAA, 0, 0, 0])

    def test_div_table(self):
        cases = [  # (op, rs, rt) -> (hi, lo), from the psx-spx divide-error table and ordinary cases
            ("div", 7, 0, 7, 0xFFFFFFFF), ("div", 0xFFFFFFF9, 0, 0xFFFFFFF9, 1),
            ("div", 0x80000000, 0xFFFFFFFF, 0, 0x80000000), ("divu", 5, 0, 5, 0xFFFFFFFF),
            ("div", 0xFFFFFFF9, 2, 0xFFFFFFFF, 0xFFFFFFFD),  # -7 / 2 = -3 rem -1 (truncates toward zero)
            ("divu", 0xFFFFFFF9, 2, 1, 0x7FFFFFFC),
        ]
        for op, a, b, hi, lo in cases:
            st, _ = run([A.r(op, 1, 2)], {1: a, 2: b})
            self.assertEqual((st.hi, st.lo), (hi, lo), (op, hex(a), hex(b)))

    def test_mult(self):
        st, _ = run([A.r("mult", 1, 2)], {1: 0xFFFFFFFE, 2: 3})  # -2 * 3 = -6
        self.assertEqual((st.hi, st.lo), (0xFFFFFFFF, 0xFFFFFFFA))
        st, _ = run([A.r("multu", 1, 2)], {1: 0xFFFFFFFF, 2: 0xFFFFFFFF})
        self.assertEqual((st.hi, st.lo), (0xFFFFFFFE, 1))

    def test_delay_slot_runs_and_jal_link(self):
        # jal outside (0x3000); slot reads $ra -> already B+8
        st, pc = run([A.j("jal", 0x3000), A.r("addu", 31, 0, 2)], {})
        self.assertEqual((st.r[31], st.r[2], pc), (B + 8, B + 8, 0x3000))

    def test_bltzal_links_even_when_not_taken(self):
        st, pc = run([A.regimm("bltzal", 1, 4), A.nop(), A.i("addiu", 0, 5, 1)], {1: 1})
        self.assertEqual((st.r[31], st.r[5], pc), (B + 8, 1, B + 12))

    def test_taken_branch_executes_slot(self):
        st, pc = run([A.i("beq", 0, 0, 3), A.i("addiu", 0, 1, 5), A.i("addiu", 0, 2, 9), A.nop()], {})
        self.assertEqual((st.r[1], st.r[2], pc), (5, 0, B + 4 + 12))  # target = pc+4+3*4 = B+16 = end

    def test_jalr_reads_target_before_link(self):
        st, pc = run([A.r("jalr", 5, 0, 5), A.nop()], {5: 0x3000})  # jalr r5,r5
        self.assertEqual((pc, st.r[5]), (0x3000, B + 8))

    def test_branch_reads_old_value_after_load(self):
        # cfc2 r2,r31 ; bltz r2 -> sees OLD r2 (5, not negative): not taken, slot runs, falls through
        st, pc = run([A.cfc2(2, 31), A.regimm("bltz", 2, 2), A.i("addiu", 0, 6, 1), A.i("addiu", 0, 7, 1)],
                     {2: 5}, cop2c={31: 0x80000000})
        self.assertEqual((st.r[6], st.r[7], st.r[2]), (1, 1, 0x80000000))

    def test_overflow_exception(self):
        st, pc = run([A.i("addi", 1, 2, 1)], {1: 0x7FFFFFFF})
        self.assertEqual((pc, st.exc), (None, (12, B, 0)))
        st, pc = run([A.i("addiu", 1, 2, 1)], {1: 0x7FFFFFFF})
        self.assertEqual((st.r[2], st.exc), (0x80000000, None))

    def test_break_code_is_division_check(self):
        st, pc = run([A.brk(0x1C00)], {})  # `break 7` as emitted by gcc: code field 7<<10
        self.assertEqual(st.exc, (9, B, 0x1C00))

    def test_rfe(self):
        st, _ = run([A.rfe()], {}, cop0={12: 0x3A})
        self.assertEqual(st.cop0[12], 0x3E)

    def test_shifts(self):
        st, _ = run([A.r("sra", 0, 1, 2, 4), A.r("srlv", 3, 1, 4), A.r("srav", 3, 1, 5)], {1: 0x80000010, 3: 33})
        self.assertEqual((st.r[2], st.r[4], st.r[5]), (0xF8000001, 0x40000008, 0xC0000008))  # shift amount 33 & 31 = 1

    def test_slt_sltu_and_immediates(self):
        st, _ = run([A.r("slt", 1, 2, 3), A.r("sltu", 1, 2, 4), A.i("sltiu", 2, 5, 0xFFFF), A.i("slti", 2, 6, 0xFFFF)],
                    {1: 0xFFFFFFFF, 2: 1})
        self.assertEqual((st.r[3], st.r[4], st.r[5], st.r[6]), (1, 0, 1, 0))  # sltiu: imm sign-extends to 0xFFFFFFFF


if __name__ == "__main__":
    unittest.main()
