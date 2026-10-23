"""Differential test of runtime/gte.c against an independent implementation (queue P4-1, finding 0024): random register files and every
command word shape; all 64 registers must agree after each command. Skips when the reference has not been built
(python3 -I tools/gte_ref/build_ref.py). Seeded, deterministic."""
import ctypes
import os
import random
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)
from tools.recomp import oracle  # noqa: E402

from tools.recomp import hostenv  # noqa: E402
EXT = hostenv.shlib_ext()
REF = os.path.join(ROOT, "work", "refs", "libgteref" + EXT)

CMDS = [0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1B, 0x1C, 0x1E, 0x20, 0x28, 0x29, 0x2A, 0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F]


def rnd32(r, kind):
    if kind == 0:
        return r.getrandbits(32)
    if kind == 1:                                   # small values, both signs
        return r.randint(-0x300, 0x300) & 0xFFFFFFFF
    if kind == 2:                                   # typical fixed-point magnitudes
        return (r.randint(-0x1000, 0x1000) & 0xFFFF) | ((r.randint(-0x1000, 0x1000) & 0xFFFF) << 16)
    return r.choice([0, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x00008000, 0xFFFF8000, 0x00007FFF, 0x1000, 0xFFFF0000, 0x0000FFFF])


@unittest.skipUnless(os.path.exists(REF), "reference not built: python3 -I tools/gte_ref/build_ref.py")
class T(unittest.TestCase):
    def test_random_commands(self):
        lib = oracle.load_io_lib()
        ref = ctypes.CDLL(REF)
        ref.ref_read.restype = ctypes.c_uint32
        ref.ref_read.argtypes = [ctypes.c_uint]
        ref.ref_write.argtypes = [ctypes.c_uint, ctypes.c_uint32]
        ref.ref_exec.argtypes = [ctypes.c_uint32]
        d, c = (ctypes.c_uint32 * 32)(), (ctypes.c_uint32 * 32)()
        da, ca = ctypes.addressof(d), ctypes.addressof(c)
        r = random.Random(20261017)
        total = bad = 0
        for it in range(int(os.environ.get("GTE_CASES", "20000"))):
            ref.ref_reset()
            for k in range(32):
                d[k] = 0
                c[k] = 0
            kind = r.randint(0, 3)
            for n in range(64):
                if n in (29, 31, 63):               # read-only / derived
                    continue
                v = rnd32(r, r.choice([kind, kind, 3]))
                if n == 62 - 0 and False:
                    pass
                ref.ref_write(n, v)
                if n < 32:
                    lib.gte_core_mtc2(da, ca, n, v)
                else:
                    lib.gte_core_ctc2(da, ca, n - 32, v)
            cmd = r.choice(CMDS)
            w = (r.getrandbits(5) << 20) | (r.getrandbits(1) << 19) | (r.getrandbits(2) << 17) | (r.getrandbits(2) << 15) | (r.getrandbits(2) << 13) | (r.getrandbits(1) << 10) | cmd
            if cmd not in (0x12, 0x2D, 0x2E):
                w &= ~((3 << 17) | (3 << 15) | (3 << 13)) if cmd != 0x12 else w
            ref.ref_exec(w)
            self.assertEqual(lib.gte_core_cmd(da, ca, w), 1)
            total += 1
            for n in range(64):
                want = ref.ref_read(n)
                got = lib.gte_core_mfc2(da, ca, n) if n < 32 else lib.gte_core_cfc2(da, ca, n - 32)
                if want != got:
                    bad += 1
                    if bad < 6:
                        print("MISMATCH it=%d cmd=%02x word=%08x reg %d: ref %08x mine %08x" % (it, cmd, w, n, want, got))
                    break
        self.assertEqual(bad, 0, "%d of %d commands differ" % (bad, total))


if __name__ == "__main__":
    unittest.main()
