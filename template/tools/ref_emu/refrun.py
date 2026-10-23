#!/usr/bin/env python3
"""run the reference emulator (local) with a press script and dump frames: refrun.py FRAMES 'btn@lo-hi,...' 'f1,f2,...' ABSPREFIX"""
import os
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refenv
frames, presses, dumps, prefix = sys.argv[1:5]
cmd = [refenv.HOST, refenv.CORE, refenv.CUE, "--frames", frames]
for p in presses.split(","):
    if p: cmd += ["--press", p]
for f in dumps.split(","):
    if f: cmd += ["--dump", "%s:%s%s.ppm" % (f, prefix, f)]
subprocess.run(cmd, cwd=refenv.HOST_DIR, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL, timeout=1500)
