#!/usr/bin/env python3
"""tools/disc_unpack.py end to end on synthetic discs (no game data): cooked + shared + raw (form 2) files must read back identical through runtime/sectors.c.
usage: python3 -I tests/runtime/test_unpack.py     (called by tests/run_all.py)"""
import os, random, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))
import hostenv  # noqa: E402

def run(*a, **k):
    r = subprocess.run(list(a), cwd=ROOT, capture_output=True, text=True, **k)
    if r.returncode: sys.exit("FAILED: %s\n%s%s" % (" ".join(a), r.stdout, r.stderr))
    return r.stdout

def main():
    rnd = random.Random(7)
    with tempfile.TemporaryDirectory() as t:
        common = [rnd.randbytes(2048) for _ in range(40)]
        def disc(n, extra):
            blob = b"".join(common[:20]) + b"".join(rnd.randbytes(2048) for _ in range(5)) + b"".join(common[20:]) + extra
            for name, data in (("BOOT.EXE", b"boot%d" % n + bytes(3000)), ("BIG.DAT", blob), ("MOVIE.STR", rnd.randbytes(2048 * 6 + 100))):
                open(os.path.join(t, name if n == 1 else "x" + name), "wb").write(data)
            fs = [os.path.join(t, "BOOT.EXE" if n == 1 else "xBOOT.EXE"), os.path.join(t, "BIG.DAT" if n == 1 else "xBIG.DAT"), os.path.join(t, "MOVIE.STR" if n == 1 else "xMOVIE.STR")]
            img = os.path.join(t, "d%d.bin" % n)
            run(sys.executable, "-I", "tools/mkdisc.py", img, *fs)
            return img
        d1, d2 = disc(1, b""), disc(2, rnd.randbytes(3000))
        for img in (d1, d2):                                      # make MOVIE.STR form 2 (submode bit 5): it can only be stored raw
            raw = bytearray(open(img, "rb").read())
            total = len(raw) // 2352
            for sec in range(total - 7, total):                  # MOVIE.STR is the last file: 7 sectors
                raw[sec * 2352 + 18] |= 0x20; raw[sec * 2352 + 22] |= 0x20
            open(img, "wb").write(raw)
        out = os.path.join(t, "out")
        log = run(sys.executable, "-I", "tools/disc_unpack.py", out, d1, d2)
        m2 = open(os.path.join(out, "d2", "disc.map")).read()
        assert "../d1/" in m2, "disc 2 should reference disc 1's data:\n" + m2
        assert "BIG.DAT.new" in m2 and "MOVIE.STR.raw" in m2, m2
        assert "shared stretches" in log, log
        exe = os.path.join(t, hostenv.exe("test_sectors"))
        run(hostenv.CC, "-Wall", "-Wextra", "-Werror", "-std=gnu11", "-Iruntime", "tests/runtime/test_sectors.c", "runtime/sectors.c", "-o", exe)
        for n, img in ((1, d1), (2, d2)):
            sectors = os.path.getsize(img) // 2352
            print(run(exe, img, os.path.join(out, "d%d" % n), str(sectors)).strip())
        out2 = os.path.join(t, "out2")                            # --standalone: disc 2 must not reference disc 1
        run(sys.executable, "-I", "tools/disc_unpack.py", "--standalone", out2, d1, d2)
        assert "../d1/" not in open(os.path.join(out2, "d2", "disc.map")).read()
        print(run(exe, d2, os.path.join(out2, "d2"), str(os.path.getsize(d2) // 2352)).strip())
    print("test_unpack OK")

if __name__ == "__main__":
    main()
