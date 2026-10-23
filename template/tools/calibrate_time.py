#!/usr/bin/env python3
"""Pick the virtual-frame length (game/game.json "yield_period", taken backward branches per frame) for a title: builds and runs the game with several candidates and reports, for each, whether the
SDK's own timeouts fired (CD driver "timeout" messages), how far the run got, and how many display-start flips (frames shown) happened. The two ports needed 4000 and 40000.
usage: python3 -I tools/calibrate_time.py [--ticks N] [--press btn@lo-hi ...] [--candidates 2000,4000,...]
Rule of thumb (observed on the two ports, docs 05): a value that is too LARGE makes the SDK's own timeouts fire (a CD driver 'timeout' message: its wait counts frames, and one virtual frame then holds too much work), a value that is too SMALL runs the title slowly in virtual time; take the largest candidate with zero driver timeouts and no fatal, then confirm with the reference emulator (frame at which a known screen appears, tools/ref_emu). The result is a tuning constant, recorded in game.json with this table in a finding."""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, ROOT)
from tools.recomp import gamecfg, hostenv  # noqa: E402

ticks = sys.argv[sys.argv.index("--ticks") + 1] if "--ticks" in sys.argv else "6000"
cands = [int(x) for x in (sys.argv[sys.argv.index("--candidates") + 1] if "--candidates" in sys.argv else "2000,4000,8000,20000,40000,80000").split(",")]
press = [x for i, x in enumerate(sys.argv) if i and sys.argv[i - 1] == "--press"]
cfgp = os.path.join(gamecfg.GAME_DIR, "game.json")
saved = open(cfgp).read()
rows = []
try:
    for p in cands:
        c = json.loads(saved)
        c["yield_period"] = p
        json.dump(c, open(cfgp, "w"), indent=2)
        b = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "recomp", "build_game.py"), "-O1"], capture_output=True, text=True, env=dict(os.environ, PS1_GAME_OUT="ps1_game_cal"))
        if b.returncode:
            rows.append((p, "build failed", "", 0)); continue
        cmd = [os.path.join(ROOT, "build", hostenv.exe("ps1_game_cal")), gamecfg.DISC, "--ticks", ticks, "--card", os.path.join(ROOT, "work", "cal.mcr")]
        for x in press:
            cmd += ["--press", x]
        env = dict(os.environ, PS1_FLIPLOG="1")
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=1800)
        out = r.stdout + r.stderr
        timeouts = len(re.findall(r"timeout", out, re.I))
        flips = len(re.findall(r"\[flip\]", out))
        m = re.findall(r"msg (.*)", out)
        rows.append((p, "ok" if m and "tick limit" in m[-1] else "FATAL", (m[-1][:80] if m else ""), timeouts, flips))
finally:
    open(cfgp, "w").write(saved)
print("| yield_period | run | driver timeouts | display flips in %s ticks | last message |\n|---|---|---|---|---|" % ticks)
for r in rows:
    print("| %d | %s | %s | %s | %s |" % (r[0], r[1], r[3] if len(r) > 3 else "", r[4] if len(r) > 4 else "", r[2]))
