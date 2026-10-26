"""Static scan of the code that lives in the disc archive (not in the boot executable): python3 -I tools/overlay_scan.py IMG [--funcs build/game/gen_slus.funcs] [--cov RUN.json ...] [--out report.md]
Observed basis: a disc archive can hold uncompressed MIPS code. A 2 KiB sector counts as code when it has >= 2 stack-frame prologues (addiu sp,sp,-N) and >= 2 'jr ra'; runs with gaps of up to 3 sectors are merged.
Everything below is INFERRED from linear decoding (a data sector can look like code): it lists (1) which functions of the boot executable the archive code calls and which of those no run entered,
(2) which hardware registers (0x1F80xxxx) the archive code addresses directly. Nothing here says the code is correct, only what it reaches."""
import json, struct, sys, collections

def sectors(path):
    f = open(path, "rb"); i = 0
    while True:
        b = f.read(2048)
        if len(b) < 2048: return
        yield i, struct.unpack("<512I", b); i += 1

def is_code(w):
    pro = sum(1 for x in w if (x >> 16) == 0x27bd and (x & 0x8000))
    ret = sum(1 for x in w if x == 0x03e00008)
    return pro >= 2 and ret >= 2

def main():
    a = sys.argv[1:]
    img = a[0]
    funcs = set(); cov = set()
    if "--funcs" in a: funcs = {int(x, 16) for x in open(a[a.index("--funcs") + 1]).read().split()}
    for k, v in enumerate(a):
        if v == "--cov": cov |= set(json.load(open(a[k + 1])).get("functions", []))
    calls = collections.Counter(); regs = collections.Counter(); ncode = 0; runs = []
    for i, w in sectors(img):
        if not is_code(w): continue
        ncode += 1
        if runs and i - runs[-1][1] <= 4: runs[-1][1] = i
        else: runs.append([i, i])
        lui = {}
        for x in w:
            op = x >> 26
            if op == 3: calls[((x & 0x3FFFFFF) << 2) | 0x80000000] += 1
            if op == 0x0F: lui[(x >> 16) & 31] = x & 0xFFFF
            elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B):
                r = (x >> 21) & 31
                if lui.get(r) == 0x1F80:
                    s = x & 0xFFFF; s = s - 65536 if s & 0x8000 else s
                    regs[(0x1F800000 + s) & ~7] += 1
    out = ["archive %s: %d code sectors in %d runs" % (img, ncode, len(runs))]
    for lo, hi in runs:
        if hi - lo < 3: continue
        out.append("  sectors %d-%d (offset 0x%x, %d KiB)" % (lo, hi, lo * 2048, (hi - lo + 1) * 2))
    ext = {c: n for c, n in calls.items() if c in funcs}
    unseen = sorted(((n, c) for c, n in ext.items() if (not cov) or c not in cov), reverse=True)
    out.append("calls into boot-executable functions: %d distinct targets; %s" % (len(ext), ("%d never entered by the runs given" % len(unseen)) if cov else "(no --cov given)"))
    for n, c in unseen[:60]: out.append("  %08x  called from %d sites" % (c, n))
    out.append("direct hardware register groups addressed (lui 0x1F80 + access): %d" % len(regs))
    for r, n in sorted(regs.items()): out.append("  %08x x%d" % (r, n))
    text = "\n".join(out)
    if "--out" in a: open(a[a.index("--out") + 1], "w").write(text + "\n")
    print(text)

main()
