#!/usr/bin/env python3
"""Run K random-walker branches of the REFERENCE in parallel from a savestate and write one contact sheet: refbatch.py IN_STATE FRAME_IN FRAMES SEEDBASE OUTDIR [K=8]
env WALK_SET (default up,down,left,right,up,right) and WALK_DIR (share of direction holds, default 0.7) shape the walkers. Each branch's end state is OUTDIR/w<seed>.st (continue from the one that got somewhere).
Output: OUTDIR/sheet.png (5 evenly spaced pictures per seed, in seed order) and the pictures OUTDIR/w<seed>_<frame>.ppm."""
import glob
import os
import re
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
st, fin, n, seedbase, out = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), os.path.abspath(sys.argv[5])
k = int(sys.argv[6]) if len(sys.argv) > 6 else 8
os.makedirs(out, exist_ok=True)
procs = [subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "refwalk.py"), st, fin, n, str(seedbase + i), os.path.join(out, "w%d_" % (seedbase + i))]) for i in range(1, k + 1)]
for p in procs:
    p.wait()
pics = []
for i in range(1, k + 1):
    fs = sorted(glob.glob(os.path.join(out, "w%d_*.ppm" % (seedbase + i))), key=lambda f: int(re.search(r"_(\d+)\.ppm", f).group(1)))
    pics += [fs[int(j * len(fs) / 5)] for j in range(5)] if fs else []
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "sheet.py"), os.path.join(out, "sheet.png"), "10", "3"] + pics, check=True)
print("sheet:", os.path.join(out, "sheet.png"), "(%d seeds x 5 pictures, 10 per row)" % k)
