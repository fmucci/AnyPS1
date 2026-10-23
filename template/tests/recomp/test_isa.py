"""Decoder checks: hand-written encodings (MIPS manual / words observed in the game binaries) and a scan of real code."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from tools.recomp.isa import decode, gpr_reads, gpr_write  # noqa: E402


def _main_exe():
    """Path of the main executable of the instance's game, or None (no game/game.json or no disc files): the disc-dependent tests skip themselves."""
    try:
        from tools.recomp import gamecfg
        p = os.path.join(gamecfg.DISC_DIR, gamecfg.MAIN_EXE)
        return p if os.path.exists(p) else None
    except SystemExit:
        return None

KNOWN = [
    (0x00000000, "sll"), (0x03E00008, "jr"), (0x27BDFFE0, "addiu"), (0xAFBF0014, "sw"), (0x3C028009, "lui"),
    (0x0C002000, "jal"), (0x00221821, "addu"), (0x0007000D, "break"), (0x0000000C, "syscall"), (0x4842F800, "cfc2"),
    (0xC9850004, "lwc2"), (0x4A180001, "cop2"), (0x04400007, "bltz"), (0x04410007, "bgez"), (0x04500007, "bltzal"),
    (0x04510007, "bgezal"), (0x40026000, "mfc0"), (0x40826000, "mtc0"), (0x42000010, "rfe"), (0x48420000, "cfc2"),
    (0x48820000, "mtc2"), (0x44000000, "illegal"), (0x8C440000, "lw"),
    (0x88440003, "lwl"), (0x98440000, "lwr"), (0xA8440003, "swl"), (0xB8440000, "swr"), (0x7C000000, "illegal"),
]


class T(unittest.TestCase):
    def test_known_encodings(self):
        for w, n in KNOWN:
            self.assertEqual(decode(w).name, n, hex(w))

    def test_fields(self):
        i = decode(0x27BDFFE0)  # addiu sp,sp,-32
        self.assertEqual((i.rs, i.rt, i.simm), (29, 29, -32))
        self.assertEqual(gpr_reads(i), {29})
        self.assertEqual(gpr_write(i), 29)
        b = decode(0x1440FFFE, 0x1000)  # bne v0,zero,-2 words
        self.assertEqual(b.branch_target(), 0x1000 + 4 - 8)
        self.assertEqual(decode(0x0C002000, 0x80010000).branch_target(), 0x80008000)  # (pc+4)&0xf0000000 | 0x2000<<2
        brk = decode(0x0007000D)
        self.assertEqual(brk.code, 0x1C00)  # psx-spx: BRK(1C00h) = division by zero check

    def test_writes_to_zero_are_none(self):
        self.assertIsNone(gpr_write(decode(0x00000000)))

    @unittest.skipUnless(_main_exe(), "needs game/game.json and the disc files")
    def test_real_code_decodes(self):
        import struct
        d = open(_main_exe(), "rb").read()
        pc, gp, base, size = struct.unpack("<IIII", d[0x10:0x20])
        words = struct.unpack("<%dI" % (size // 4), d[0x800:0x800 + size])
        lo = (pc - base) // 4                                            # the first 0x400 bytes from the entry point are the start-up code
        code = words[lo:lo + 0x100]
        ill = [hex(base + 4 * (lo + k)) for k, w in enumerate(code) if decode(w).name == "illegal"]
        self.assertEqual(ill, [])


if __name__ == "__main__":
    unittest.main()
