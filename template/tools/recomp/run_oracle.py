#!/usr/bin/env python3
"""Run one executable in the oracle with its hooks until it stops or hits something unproven.
usage: python3 -I tools/recomp/run_oracle.py EXE_NAME [--steps N] [--low LO-HI ...]   (reads og-files/disc/EXE_NAME and the disc image of game/game.json)"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, ROOT)
from tools.recomp import funcs, hooks, oracle  # noqa: E402
from tools.recomp import gamecfg  # noqa: E402


def main():
    exe = sys.argv[1]
    steps = int(sys.argv[sys.argv.index("--steps") + 1]) if "--steps" in sys.argv else 5_000_000
    path = os.path.join(gamecfg.DISC_DIR, exe)
    img = funcs.Image(path)
    o = oracle.Oracle()
    o.mem.low_ranges.append((0x0, 0x1000))       # kernel exception-vector area and the exception stack of event callbacks (zeros; docs 04, 05)
    for lo, hi, _why in gamecfg.LOW_RAM:         # more ranges this title reads, each with its evidence in game/game.json
        o.mem.low_ranges.append((lo, hi))
    o.load_exe(path)
    h = hooks.Hle(gamecfg.DISC)
    hooks.install(o, h, exe, img)
    h.chain = "--chain" in sys.argv
    for i, a in enumerate(sys.argv):
        if a == "--press":                      # --press button[+button]@first-last (virtual frames)
            names, span = sys.argv[i + 1].split("@")
            lo, hi = span.split("-")
            h.pad_script.append((int(lo), int(hi), hooks.Hle.pad_word(*names.split("+"))))
    h.current = exe
    hooks.install_bios(o, h, img)
    o.attach_hw(gamecfg.DISC)
    import tempfile
    h.card = o.io.card_new(os.path.join(tempfile.mkdtemp(), 'card.mcr').encode())
    try:
        print(o.run(steps))
    except oracle.Fatal as e:
        print("STOP:", e, "| pc=%08x ra=%08x steps=%d" % (o.st.pc, o.st.r[31], o.steps))
        if "--regs" in sys.argv:
            print(" ".join("r%d=%08x" % (i, v) for i, v in enumerate(o.st.r)))
            sp = o.st.r[29]
            print("stack:", " ".join("%08x" % o.mem.load(sp + 4 * k, 4) for k in range(48)))
    cnt = (__import__("ctypes").c_uint64 * 8)()
    o.io.io_gpu_counters(o.io_ctx, cnt)
    print("gpu counters [words, prims, uploads, fills, dma2, dispstart, stat, ot]:", [hex(x) if i >= 5 else x for i, x in enumerate(cnt)])
    if "--ppm" in sys.argv:
        o.io.io_dump_ppm(o.io_ctx, sys.argv[sys.argv.index("--ppm") + 1].encode(), 0, 0, 1024, 512)
    if "--display" in sys.argv:
        dw, dh, rgb, on = o.present()
        open(sys.argv[sys.argv.index("--display") + 1], "wb").write(b"P6\n%d %d\n255\n" % (dw, dh) + rgb)
        print("display %dx%d enabled=%s" % (dw, dh, on))
    for i, a in enumerate(sys.argv):             # --mem ADDR:LEN (hex) prints guest memory after the run
        if a == "--mem":
            ma, ml = [int(v, 16) for v in sys.argv[i + 1].split(":")]
            print("mem %08x:" % ma, " ".join("%02x" % o.mem.load(ma + k, 1) for k in range(ml)))
    if "--hle-log" in sys.argv:                 # every HLE/BIOS call of the run in order (name and argument registers), one line each
        for e in o.trace:
            if e[0] == "hle":
                print("hle", e[1], " ".join("%08x" % v for v in e[2:]))
    print("events", len(o.trace), "stdout", bytes(h.stdout)[:200])


if __name__ == "__main__":
    main()
