#!/usr/bin/env python3
"""Smoke-run many standalone PS-X EXE programs (e.g. the public ps1-tests release) through the whole agent flow: mkdisc -> init_game -> find_hooks -> build -> run, one after the other in THIS instance
(it rewrites game/game.json for each program and restores it at the end). Reports per program: build ok?, final message, ticks reached, coverage groups touched.
usage: python3 -I tools/selftest_public.py DIR_WITH_EXES [--ticks N] [--only SUBSTR] [--md OUT.md]
A fatal other than "tick limit reached" is a gap in the generic model or a program that needs input/disc features: each one is a work item for docs/port-queue.md."""
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
root_dir = sys.argv[1]
ticks = sys.argv[sys.argv.index("--ticks") + 1] if "--ticks" in sys.argv else "1500"
only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else ""
md = sys.argv[sys.argv.index("--md") + 1] if "--md" in sys.argv else None
PY = [sys.executable, "-I"]
cfgp = os.path.join(ROOT, "game", "game.json")
saved = open(cfgp).read() if os.path.exists(cfgp) else None
work = os.path.join(ROOT, "work", "selftest")
os.makedirs(work, exist_ok=True)
rows = []
exes = sorted(os.path.join(d, f) for d, _s, fs in os.walk(root_dir) for f in fs if f.lower().endswith(".exe") and "diffvram" not in f and only in os.path.join(d, f))
for exe in exes:
    name = re.sub(r"[^a-z0-9]+", "", os.path.relpath(exe, root_dir).lower().replace(".exe", ""))[:20]
    with open(exe, "rb") as f:
        if f.read(8) != b"PS-X EXE":
            continue
    img = os.path.join(work, name + ".bin")
    subprocess.run(PY + [os.path.join(ROOT, "tools", "mkdisc.py"), img, exe], check=True, capture_output=True)
    shutil.rmtree(os.path.join(ROOT, "og-files", "disc"), ignore_errors=True)
    r = subprocess.run(PY + [os.path.join(ROOT, "tools", "init_game.py"), img, "--name", name], capture_output=True, text=True)
    status, msg, cov = "init failed", r.stdout[-200:] + r.stderr[-200:], ""
    if r.returncode == 0:
        open(os.path.join(ROOT, "game", "hooks.csv"), "w").write("exe,address,name,param,param2,masked_signature_sha256_prefix\n")
        subprocess.run(PY + [os.path.join(ROOT, "tools", "recomp", "find_hooks.py"), "--write"], capture_output=True)
        b = subprocess.run(PY + [os.path.join(ROOT, "tools", "recomp", "build_game.py"), "-O1"], capture_output=True, text=True)
        if b.returncode != 0:
            status, msg = "build failed", (b.stdout + b.stderr)[-300:].replace("\n", " ")
        else:
            c = os.path.join(work, name + ".cov.json")
            env = dict(os.environ, PS1_COVERAGE=c)
            run = subprocess.run([os.path.join(ROOT, "build", "ps1_game"), os.path.join(ROOT, "og-files", name + "-d1.bin"), "--ticks", ticks, "--card", os.path.join(work, name + ".mcr"), "--ppm", os.path.join(work, name + ".ppm")], capture_output=True, text=True, env=env, timeout=600)
            out = run.stdout + run.stderr
            m = re.findall(r"msg (.*)", out)
            msg = m[-1][:170] if m else "(no message)"
            status = "ok" if "tick limit reached" in msg else "FATAL"
            if os.path.exists(c):
                j = json.load(open(c))
                cov = ", ".join("%s:%d" % (g, len(v)) for g, v in j.items() if g != "functions" and v)
    rows.append((os.path.relpath(exe, root_dir), status, msg, cov))
    print("%-44s %-12s %s" % (rows[-1][0], rows[-1][1], rows[-1][2][:110]), flush=True)
if saved is not None:
    open(cfgp, "w").write(saved)
if md:
    with open(md, "w") as f:
        f.write("# Public test programs through the agent flow (%s ticks each)\n\n| program | result | last message | features touched (group:distinct ids) |\n|---|---|---|---|\n" % ticks)
        for r in rows:
            f.write("| %s | %s | %s | %s |\n" % tuple(x.replace("|", "/") for x in r))
