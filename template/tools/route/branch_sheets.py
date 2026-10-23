#!/usr/bin/env python3
"""Contact sheets of every explorer branch of a session directory (tools/route/ours_session.py): one PNG per branch with up to N evenly spaced pictures, plus a one-line summary per branch
(last message of its log, number of distinct pictures = a crude novelty measure: a branch stuck on one screen has 1-2).
usage: python3 -I tools/route/branch_sheets.py DIR [N=40]"""
import glob
import hashlib
import os
import re
import subprocess
import sys

d = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
here = os.path.dirname(os.path.abspath(__file__))
prefixes = sorted({re.sub(r"\d+\.ppm$", "", os.path.basename(f)) for f in glob.glob(os.path.join(d, "*.ppm"))})
for p in prefixes:
    fs = sorted(glob.glob(os.path.join(d, p + "*.ppm")), key=lambda f: int(re.search(r"(\d+)\.ppm$", f).group(1)))
    if not fs:
        continue
    pick = [fs[int(i * len(fs) / min(n, len(fs)))] for i in range(min(n, len(fs)))]
    out = os.path.join(d, "sheet_%s.png" % p.rstrip("_"))
    subprocess.run([sys.executable, os.path.join(here, "..", "ref_emu", "sheet.py"), out, "10", "3"] + pick, check=True)
    distinct = len({hashlib.md5(open(f, "rb").read()[:60000]).hexdigest() for f in fs})
    log = os.path.join(d, p + "log.txt")
    msg = ""
    if os.path.exists(log):
        m = re.findall(r"msg (.*)", open(log, errors="replace").read())
        msg = m[-1][:100] if m else ""
    print("%-10s %3d pictures, %3d distinct (first 60 KB)  %s  -> %s" % (p, len(fs), distinct, msg, out))
