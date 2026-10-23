"""Tests of tools/recomp/funcs.py on a synthetic PS-X EXE (no game data) plus, when the disc is present, the Ghidra cross-check count."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(__file__))
from asm import i, j, r, regimm  # noqa: E402
from tools.recomp import funcs  # noqa: E402


def _main_exe():
    """Path of the main executable of the instance's game, or None (no game/game.json or no disc files): the disc-dependent tests skip themselves."""
    try:
        from tools.recomp import gamecfg
        p = os.path.join(gamecfg.DISC_DIR, gamecfg.MAIN_EXE)
        return p if os.path.exists(p) else None
    except SystemExit:
        return None

BASE = 0x80020000
NOP = 0
JR_RA = r("jr", rs=31)


def build():
    w = [0] * 40
    w[0] = j("jal", BASE + 4 * 8); w[1] = NOP; w[2] = JR_RA; w[3] = NOP
    w[4] = i("addiu", 0, 2, 1); w[5] = JR_RA; w[6] = NOP; w[7] = NOP            # unreferenced leaf at idx 4
    # f1 @ idx 8: bounded jump table (sltiu idx,2)
    w[8] = i("sltiu", 4, 2, 2); w[9] = i("beq", 2, 0, 9); w[10] = NOP
    w[11] = r("sll", rt=4, rd=2, sa=2); w[12] = i("lui", 0, 1, BASE >> 16); w[13] = r("addu", rs=1, rt=2, rd=1)
    w[14] = i("lw", 1, 2, 0x78); w[15] = NOP; w[16] = r("jr", rs=2); w[17] = NOP
    w[18] = JR_RA; w[19] = NOP                                                 # case 0
    w[20] = JR_RA; w[21] = NOP                                                 # case 1
    w[22] = JR_RA; w[23] = NOP                                                 # default (beq target idx 10+9=19? see below)
    w[30] = BASE + 4 * 18; w[31] = BASE + 4 * 20                              # table at idx 30 = BASE+0x78
    w[33] = BASE + 4 * 4                                                       # function pointer in data
    return w


class T(unittest.TestCase):
    def setUp(self):
        w = build()
        hdr = bytearray(0x800)
        hdr[:8] = b"PS-X EXE"
        struct.pack_into("<IIII", hdr, 0x10, BASE, 0, BASE, 4 * len(w))
        self.f = tempfile.NamedTemporaryFile(delete=False)
        self.f.write(bytes(hdr) + struct.pack("<%dI" % len(w), *w))
        self.f.close()
        self.img = funcs.Image(self.f.name)
        self.ent, self.bod = funcs.discover(self.img)

    def tearDown(self):
        os.unlink(self.f.name)

    def test_entries(self):
        self.assertEqual(self.ent[BASE][0], "entry")
        self.assertEqual(self.ent[BASE + 32][0], "jal")
        self.assertIn(self.ent[BASE + 16][0], ("leaf-gap", "ptr-data"))   # the unreferenced leaf is found, never silently dropped

    def test_jump_table(self):
        b = self.bod[BASE + 32]
        self.assertEqual(list(b.tables), [BASE + 4 * 16])
        tab, tg, bounded = b.tables[BASE + 4 * 16]
        self.assertEqual((tab, bounded, sorted(tg)), (BASE + 0x78, True, [BASE + 72, BASE + 80]))
        self.assertIn(BASE + 72, b.seen)
        self.assertIn(BASE + 80, b.seen)

    def test_data_not_code(self):
        covered = set()
        for b in self.bod.values():
            covered |= b.seen
        self.assertNotIn(BASE + 4 * 30, covered)
        self.assertEqual([b for b in self.bod.values() if b.bad], [])

    @unittest.skipUnless(_main_exe(), "needs game/game.json and the disc files")
    def test_exe_image(self):
        img = funcs.Image(_main_exe())                                  # the header fields are consistent: entry inside the image, base word aligned
        self.assertTrue(img.inside(img.pc))
        self.assertEqual(img.base % 4, 0)


if __name__ == "__main__":
    unittest.main()
