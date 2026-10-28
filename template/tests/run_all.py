#!/usr/bin/env python3
"""Runs every test of the recompiler pieces, the runtime and the whole-game smoke test (queue P2-7, P2-8, P4-6). Portable driver (macOS, Linux, Windows).
   python3 -I tests/run_all.py            quick: unit tests + 1500 differential cases + mutation check
   python3 -I tests/run_all.py --long     30000 differential cases over 5 seeds
Needs a C compiler (`PS1_CC`, `CC`, `cc`, `gcc` or `clang`); the disc-dependent tests skip themselves without og-files/. `./tests/run_all.sh` calls this script."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))
import hostenv  # noqa: E402

PY = [sys.executable, "-I"]


def run(args, grep=None, tail=None):
    r = subprocess.run(args, cwd=ROOT, capture_output=bool(grep or tail), text=True)
    if grep or tail:
        lines = r.stdout.splitlines()
        if grep:
            lines = [l for l in lines if l.startswith(grep)]
        if tail:
            lines = lines[-tail:]
        print("\n".join(lines))
        sys.stderr.write(r.stderr)
    if r.returncode:
        sys.exit("FAILED: " + " ".join(args))


def main():
    long_run = "--long" in sys.argv[1:]
    run(PY + ["tests/recomp/test_isa.py"])
    run(PY + ["tests/recomp/test_spec.py"])
    if long_run:
        for s in range(1, 6):
            run(PY + ["tests/recomp/run_diff.py", "--cases", "6000", "--batch", "400", "--seed", str(s)], grep="stats")
    else:
        run(PY + ["tests/recomp/run_diff.py", "--cases", "1500", "--batch", "300", "--seed", "1"], grep="stats")
    run(PY + ["tests/recomp/mutation_check.py", "--cases", "2500"], tail=3)
    run(PY + ["tests/recomp/run_diff_interp.py", "--cases", "2000", "--seed", "1"], grep="stats")            # the interpreter tier against the spec (finding 0004)
    md = []                                                  # Markdown tables must not be split by blank lines (GitHub would render the rest of the rows as paragraphs)
    for d, dirs, files in os.walk(ROOT):
        dirs[:] = [x for x in dirs if x not in ("build", "work", "og-files", "dist", ".git", "__pycache__")]
        md += [os.path.join(d, f) for f in files if f.endswith(".md")]
    run(PY + ["tools/fix_md_tables.py", "--check"] + md)
    run(PY + ["tools/check_stubs.py"])
    run(PY + ["tests/recomp/test_funcs.py"])
    game = None                                                  # the disc-dependent checks run only in an instance that has game/game.json and the disc files
    if os.path.exists(os.path.join(ROOT, "game", "game.json")):
        sys.path.insert(0, ROOT)
        from tools.recomp import gamecfg
        game = gamecfg
    if game and os.path.isdir(game.DISC_DIR):                    # callbacks discovery cannot recognise: every code pointer must resolve to an emitted function
        run(PY + ["tools/recomp/unresolved_pointers.py", "--check"] + [os.path.join(game.DISC_DIR, n) for n, _p in game.EXES])
    # runtime unit tests: UBSan only where the compiler supports it (ASan deadlocks in its own init on the macOS toolchain used so far, ai-rules/05 pitfall 15)
    san = ["-fsanitize=undefined"] if sys.platform != "win32" else []
    rt = ["runtime/mem.c", "runtime/sys.c", "runtime/dispatch.c", "runtime/trace.c", "runtime/interp.c", "runtime/sectors.c"]
    tests = [("test_interp", ["tests/runtime/test_interp.c"] + rt + ["runtime/io.c", "runtime/gte.c", "runtime/card.c"]),
             ("test_rt", ["tests/runtime/test_rt.c"] + rt + ["runtime/io.c", "runtime/gte.c", "runtime/card.c"]),
             ("test_card", ["tests/runtime/test_card.c", "runtime/card.c"]),
             ("test_hw", ["tests/runtime/test_hw.c", "runtime/gte.c", "runtime/card.c"] + rt)]
    with tempfile.TemporaryDirectory() as t:
        for name, srcs in tests:
            exe = os.path.join(t, hostenv.exe(name))
            run([hostenv.CC, "-Wall", "-Wextra", "-Werror", "-std=gnu11"] + san + ["-g", "-Iruntime"] + srcs + ["-o", exe] + (["-lm"] if sys.platform != "win32" else []))
            run([exe])
    if os.path.isdir(os.path.join(ROOT, "work", "refs", "ps1-tests-bin")) and os.path.isdir(os.path.join(ROOT, "work", "refs", "ps1-tests")) and game is not None and os.path.exists(game.DISC):
        # public ps1-tests programs run through our pipeline; these must stay pixel-identical to the PNGs shipped with them (finding 0043; skipped unless downloaded into work/refs)
        run(PY + ["tools/ps1_tests.py", "--expect", "100", "gpu/clipping", "gpu/clut-cache", "gpu/quad", "gpu/rectangles", "gpu/texture-flip", "gpu/texture-overflow", "gpu/transparency", "gpu/triangle", "gpu/uv-interpolation", "gpu/vram-to-vram-overlap"])
    if os.path.isdir(os.path.join(ROOT, "work", "refs", "ps1-tests-bin")) and os.path.isdir(os.path.join(ROOT, "work", "refs", "ps1-tests")) and game is not None and os.path.exists(game.DISC):
        run(PY + ["tools/ps1_tests.py", "--no-fails", "dma/otc-test", "gpu/gp0-e1", "gpu/mask-bit"])          # self-checking programs: no `fail -` line
    run(PY + ["tests/runtime/test_unpack.py"])               # disc_unpack.py + runtime/sectors.c on synthetic discs (cooked, shared and raw extents)
    run(PY + ["tests/gte/test_gte_ref.py"])                # GTE against the independent reference (skips unless tools/gte_ref/build_ref.py was run)
    print("ALL RECOMP TESTS PASSED")


if __name__ == "__main__":
    main()
