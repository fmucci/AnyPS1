#!/usr/bin/env python3
"""Run OUR build from a base press script and fork explorer branches at a tick (run_game --branch), each branch with its own pictures, log and card file.
usage: python3 -I tools/route/ours_session.py OUTDIR BASE_PRESSES.txt BRANCH_TICK TICKS [--card BASE.mcr]
  OUTDIR/branches.txt  one line per branch: `PREFIX[:FIRST:STEP] press press ...` (write it by hand or with tools/route/owalk.py)
  BASE_PRESSES.txt     `--press btn@lo-hi ...` on one line (e.g. title -> Continue -> Load a saved game: the memory card is the checkpoint that survives sessions)
Branch results: OUTDIR/<prefix><tick>.ppm pictures, <prefix>log.txt (a fatal shows there), <prefix>card.mcr (saves made by that branch become new checkpoints)."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.recomp import hostenv, gamecfg  # noqa: E402

out, base, tick, ticks = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
card = sys.argv[sys.argv.index("--card") + 1] if "--card" in sys.argv else os.path.join(out, "base.mcr")
if not os.path.exists(os.path.join(out, "base.mcr")) and "--card" not in sys.argv:
    open(os.path.join(out, "base.mcr"), "ab").close()
exe = os.path.join(gamecfg.ROOT, "build", hostenv.exe(os.environ.get("PS1_GAME_OUT", "ps1_game")))
cmd = [exe, gamecfg.DISC, "--ticks", ticks, "--card", card] + open(base).read().split() + ["--branch", "%s:%s" % (tick, os.path.join(out, "branches.txt"))]
with open(os.path.join(out, "main.log"), "w") as log:
    subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
print("done:", out)
