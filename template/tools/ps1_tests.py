#!/usr/bin/env python3
"""Runs the public `ps1-tests` programs (JaCzekanski/ps1-tests, MIT) through OUR pipeline and compares the VRAM they leave with the `vram.png` shipped next to their source
(queue P4-10, AnyPS1 doc 16 B1). Provenance of those PNGs (console or emulator) is not stated by the repository: treat agreement as one more independent reference.
usage: python3 -I tools/ps1_tests.py [--ticks N] [--keep] [--expect PCT] TEST...      --expect makes the exit code 1 when a test's identical-pixel percentage is below PCT; --no-fails makes it 1 when the program printed a `fail -` line or no `pass -` line        TEST = path under the tests like gpu/triangle
Needs (all git-ignored, under work/refs/): the repository clone `ps1-tests` (expected PNGs) and the release `ps1-tests-bin/` (prebuilt .exe); the disc image og-files/ only
because the runtime opens one (the programs never read it). Output: percentage of identical VRAM pixels (5-bit colour) and the first differing cell per test."""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))
import hostenv  # noqa: E402

SRC = os.path.join(ROOT, "work", "refs", "ps1-tests")
BIN = os.path.join(ROOT, "work", "refs", "ps1-tests-bin")
sys.path.insert(0, ROOT)
from tools.recomp import gamecfg
DISC = gamecfg.DISC                       # any disc image works as the backing disc of the test programs
LAST_PCT = {}
LAST_LOG = {}


def read_png(path):
    """Minimal PNG reader (8-bit, non-interlaced, grey/RGB/RGBA/palette) -> (w, h, bytes RGB)."""
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, pal = 8, b"", None
    while pos < len(d):
        n, typ = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            assert bd in (1, 2, 4, 8) and il == 0, (bd, il)
        elif typ == b"PLTE":
            pal = body
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    raw = zlib.decompress(idat)
    bpp = ch * bd                                  # bits per pixel
    stride = (w * bpp + 7) // 8
    ch = max(1, bpp // 8)                          # filter byte distance
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        for x in range(w):
            if bd < 8:
                v = (line[(x * bd) // 8] >> (8 - bd - (x * bd) % 8)) & ((1 << bd) - 1)
                if ct == 3:
                    out += pal[v * 3:v * 3 + 3]
                else:
                    g = v * 255 // ((1 << bd) - 1)
                    out += bytes([g, g, g])
            elif ct == 3:
                i = line[x] * 3
                out += pal[i:i + 3]
            elif ct == 0:
                out += bytes([line[x]] * 3)
            elif ct in (2, 6):
                out += line[x * ch:x * ch + 3]
            else:
                out += bytes([line[x * 2]] * 3)
    return w, h, bytes(out)


def read_ppm(path):
    d = open(path, "rb").read().split(b"\n", 3)
    w, h = map(int, d[1].split())
    return w, h, d[3]


def compare_log(test, console):
    """Lines of the console output against the `psx.log` shipped with the test (the output of the original run); '' when the test has no log."""
    path = os.path.join(SRC, test, "psx.log")
    if not os.path.exists(path):
        return ""
    want = [l.rstrip() for l in open(path, encoding="latin1").read().replace("\r", "").split("\n") if l.strip()]
    got = [l.rstrip() for l in console.replace("\r", "").split("\n") if l.strip() and not l.startswith("ResetGraph:")]      # the SDK prints its start-up lines through the BIOS; the original log has none
    same = sum(1 for a, b in zip(want, got) if a == b)
    first = next((i for i, (a, b) in enumerate(zip(want, got)) if a != b), None)
    note = "; psx.log: %d of %d lines identical (ours has %d lines)" % (same, len(want), len(got))
    if first is not None:
        note += "; first difference at line %d: want %r got %r" % (first, want[first][:60], got[first][:60])
    return note


def run_one(test, ticks, keep):
    name = os.path.basename(test)
    exe = os.path.join(BIN, test, name + ".exe")
    png = os.path.join(SRC, test, "vram.png")
    if not os.path.exists(exe):
        return "no prebuilt exe"
    with tempfile.TemporaryDirectory() as t:
        gen = os.path.join(t, "gen.c")
        subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools/recomp/gen.py"), exe, gen, "--prefix", "pt", "--auto-roots"], check=True, capture_output=True)
        run = os.path.join(t, hostenv.exe("run"))
        subprocess.run([hostenv.CC, "-std=gnu11", "-O1", "-w", "-I", os.path.join(ROOT, "runtime"), "-DGEN_REGISTER=gen_register_pt", "-DGEN_HOOKS=gen_hooks_pt", "-DGEN_CODE=gen_code_pt",
                        gen, os.path.join(ROOT, "tests/runtime/run_exe.c")] + [os.path.join(ROOT, "runtime", f) for f in ("mem.c", "sys.c", "dispatch.c", "interp.c", "disc.c", "trace.c", "hle.c", "io.c", "gte.c", "card.c")] + ["-o", run], check=True)
        ppm = os.path.join(t, "vram.ppm")
        env = dict(os.environ, PS1_DEFAULT_SP="1", PS1_MAX_TICKS=str(ticks), PS1_PPM=ppm, PS1_CARD=os.path.join(t, "c.mcr"))
        r = subprocess.run([run, DISC, exe, os.path.join(t, "trace.txt")], capture_output=True, env=env, timeout=600)
        out = r.stdout.decode("latin1")
        msg = out[out.rindex("\nmsg ") + 5:].strip() if "\nmsg " in out else "no msg"
        console = ""
        if "\nstdout " in out:                                    # run_exe prints "stdout N" then the N captured console bytes
            k = out.index("\nstdout ") + 1
            nl = out.index("\n", k)
            n = int(out[k + 7:nl])
            console = out[nl + 1:nl + 1 + n]
        logres = compare_log(test, console)
        LAST_LOG[test] = (sum(1 for l in console.split("\n") if l.startswith("pass -")), sum(1 for l in console.split("\n") if l.startswith("fail -")))   # the test framework prints `pass - name` / `fail - name:line ...`
        if keep:
            os.makedirs(os.path.join(ROOT, "build", "ps1tests"), exist_ok=True)
            open(os.path.join(ROOT, "build", "ps1tests", test.replace("/", "_") + ".log"), "w").write(console)
        if not os.path.exists(ppm):
            return "no VRAM dump (%s)%s" % (msg[:100], logres)
        w, h, ours = read_ppm(ppm)
        if keep:
            os.makedirs(os.path.join(ROOT, "build", "ps1tests"), exist_ok=True)
            open(os.path.join(ROOT, "build", "ps1tests", test.replace("/", "_") + ".ppm"), "wb").write(b"P6\n%d %d\n255\n" % (w, h) + ours)
    if not os.path.exists(png):
        return "no expected image; stop: %s%s" % (msg[:200], logres)
    ew, eh, exp = read_png(png)
    assert (w, h) == (ew, eh), ((w, h), (ew, eh))
    same = tot = blank = 0
    cells = {}
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            a = ours[i:i + 3]
            b = exp[i:i + 3]
            ok = all((a[k] >> 3) == (b[k] >> 3) for k in range(3))
            if not ok and a == b"\0\0\0" and all((b[k] >> 3) == 31 for k in range(3)):      # untouched VRAM: ours is black (power-on content unknown), the capture is white
                blank += 1
                continue
            tot += 1
            same += ok
            if not ok:
                c = (x // 64, y // 64)
                cells[c] = cells.get(c, 0) + 1
    worst = sorted(cells.items(), key=lambda kv: -kv[1])[:3]
    LAST_PCT[test] = 100.0 * same / tot
    return "%.3f %% identical (%d of %d compared pixels differ; %d white-vs-black blank-VRAM pixels ignored); worst 64x64 cells %s; stop: %s%s" % (100.0 * same / tot, tot - same, tot, blank, worst, msg[:160], logres)


def main():
    args = sys.argv[1:]
    ticks = int(args[args.index("--ticks") + 1]) if "--ticks" in args else 600
    keep = "--keep" in args
    expect = float(args[args.index("--expect") + 1]) if "--expect" in args else None
    nofails = "--no-fails" in args
    tests = [a for a in args if not a.startswith("--") and not a.replace(".", "").isdigit()]
    bad = 0
    for t in tests:
        try:
            print("%-28s %s" % (t, run_one(t, ticks, keep)), flush=True)
            if expect is not None and LAST_PCT.get(t, 0.0) < expect:
                bad += 1
            if nofails and (LAST_LOG.get(t, (0, 1))[1] > 0 or LAST_LOG.get(t, (0, 1))[0] == 0):
                bad += 1
        except Exception as e:  # noqa: BLE001
            print("%-28s ERROR %s" % (t, str(e)[:200]), flush=True)
            bad += 1
    sys.exit(1 if bad else 0)


main()
