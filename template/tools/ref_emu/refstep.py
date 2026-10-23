#!/usr/bin/env python3
"""refstep.py IN_STATE|- FRAME_IN FRAME_OUT OUT_STATE 'btn@lo-hi,...' DUMP_EVERY PREFIX : resume (or start) the reference, press, dump frames, save a state
env CARD_OUT / CARD_IN: card image to write / read (needs REF_CARD1=1, see host.c)"""
import os
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refenv
st, fin, fout, sout, pr, every, pre = sys.argv[1:8]
st = st if st == "-" else os.path.abspath(st); sout = os.path.abspath(sout); pre = os.path.abspath(pre)
cmd = [refenv.HOST, refenv.CORE, refenv.CUE, "--frames", fout, "--savestate", sout]
if st != "-": cmd += ["--loadstate", "%s:%s" % (st, fin)]
if os.environ.get("CARD_OUT"): cmd += ["--savecard", os.environ["CARD_OUT"]]
if os.environ.get("CARD_IN"): cmd += ["--loadcard", os.environ["CARD_IN"]]
for p in pr.split(","):
    if p: cmd += ["--press", p]
for f in range(int(fin) + int(every) - 1 if int(every) else 0, int(fout), int(every) or 10 ** 9): cmd += ["--dump", "%d:%s%d.ppm" % (f, pre, f)]
subprocess.run(cmd, cwd=refenv.HOST_DIR, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
