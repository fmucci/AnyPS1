#!/usr/bin/env python3
"""Generate the C for one PS-X EXE (queue P2-3): one C function per discovered guest function, plus a registration routine.

usage: python3 -I tools/recomp/gen.py EXE OUT.c --prefix NAME [--hooks]   (hooks = rows of tools/recomp/hooks.csv for this executable)

Output goes under build/ (git-ignored): it contains the game's code and must never be committed (decisions/0002 section 1.2).
Functions the emitter cannot translate (HazardError / Unsupported / illegal words) are emitted as stubs that abort with the reason when
called, so a missing translation is always found loudly. Hooked addresses are not emitted (they are registered by the runtime).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.recomp import emit, funcs, hooks, isa  # noqa: E402
from tools.recomp import gamecfg  # noqa: E402


def main():
    a = sys.argv[1:]
    exe, out = a[0], a[1]
    prefix = a[a.index("--prefix") + 1]
    emit.PREFIX = prefix + "_"
    img = funcs.Image(exe)
    rows = hooks.load_table(os.path.basename(exe)) if "--hooks" in a else []
    hook_addrs = {r[0] for r in rows}
    for addr, name, param, param2, want in rows:
        i = (addr - img.base) // 4
        sig = hooks.signature(img.words[i:i + hooks.SIG_WORDS])
        if want != sig:
            sys.exit("hook %s at %08x: masked signature %s does not match the committed one (%s); refusing to bind" % (name, addr, sig, want))
    roots = []                                                  # tools/recomp/roots.csv: exe,address,evidence
    rf = gamecfg.ROOTS_CSV
    if os.path.exists(rf):
        for line in open(rf).read().splitlines()[1:]:
            c = line.split(",", 2)
            if len(c) == 3 and c[0] == os.path.basename(exe):
                roots.append(int(c[1], 16))
    ents, bodies = funcs.discover(img, hook_addrs, roots)
    if "--auto-roots" in a:                                      # unknown programs (e.g. the public ps1-tests): every unresolved code pointer becomes an entry, repeated until none is left
        for _ in range(6):
            more = funcs.unresolved_code_pointers(img, ents, bodies, tables=True, min_instr=2)         # constructor tables of the public test programs point at two-instruction functions
            if not more:
                break
            roots += sorted({v for v, _, _ in more})
            ents, bodies = funcs.discover(img, hook_addrs, roots)
    patches = {}
    pf = os.path.join(gamecfg.GAME_DIR, "patches", os.path.basename(exe) + ".csv")       # game/patches/<exe>.csv: address,word;word (self-modifying code the translator may assume)
    if os.path.exists(pf):
        for line in open(pf).read().splitlines()[1:]:
            ad, ws = line.split(",")[:2]
            patches[int(ad, 16)] = [int(x, 16) for x in ws.split(";")]
    for ad, ws in patches.items():
        if ws[0] != img.w(ad):
            sys.exit("patch at %08x: first word %08x is not the word in the image %08x" % (ad, ws[0], img.w(ad)))
    emitted, stubs, body_src, extra = [], [], [], []
    resume = funcs.resume_points(img, bodies)
    nocode = {e for e, b in bodies.items() if b.bad and b.bad[0][1] == "zero fill"}      # zero-filled memory (an overlay's destination): no function, calls to it go through rt_call and reach the interpreter tier once code is loaded there
    for rng in os.environ.get("PS1_NOCODE", "").split(","):             # debug aid: PS1_NOCODE=lo-hi[,lo-hi] leaves the functions in those ranges to the interpreter tier (observability with PS1_PCLOG)
        if "-" in rng:
            lo_s, hi_s = rng.split("-")
            nocode |= {e for e in bodies if int(lo_s, 16) <= e < int(hi_s, 16)}
    known = set(ents) - hook_addrs - nocode
    for e in sorted(bodies):
        b = bodies[e]
        if e in nocode:
            continue
        if b.bad:
            stubs.append((e, "decode problem: %s" % (b.bad[0],)))
            continue
        lo, hi = min(b.seen), b.hi
        words = [img.w(x) for x in range(lo, hi, 4)]
        for k, w in enumerate(words):                                  # a word that is not an instruction (data inside the span, or a path that falls into data): `break 0xFFFFF`; executing it stops the run with a fatal naming the address, it never silently continues
            if isa.decode(w, lo + 4 * k).name == "illegal":
                words[k] = 0x0000000D | (0xFFFFF << 6)
        tables = {site: t[1] for site, t in b.tables.items()}
        try:
            src, rep = emit.emit_fragment(words, lo, emit.fn_name(e), fn={"known": {k & 0x1FFFFFFF for k in known}, "tables": tables, "entry": e, "patches": patches})
        except (emit.HazardError, emit.Unsupported) as ex:
            stubs.append((e, "%s: %s" % (type(ex).__name__, str(ex)[:150])))
            continue
        body_src.append(src)
        emitted.append(e)
        for rp in sorted(resume.get(e, ())):
            name = emit.fn_name(e) + "_at_%08x" % rp
            try:
                rsrc, _ = emit.emit_fragment(words, lo, name, fn={"known": {k & 0x1FFFFFFF for k in known}, "tables": tables, "entry": rp, "patches": patches})
            except (emit.HazardError, emit.Unsupported) as ex:
                stubs.append((rp, "resume point: %s" % ex))
                continue
            body_src.append(rsrc)
            extra.append((rp, name))
    # a call target that is neither emitted nor hooked would be a direct call to a missing symbol: those go through rt_call instead
    lines = ['/* GENERATED by tools/recomp/gen.py from %s -- game code, never commit */' % os.path.basename(exe),
             '#include <stdint.h>', '#include "cpu.h"', '#include "rt.h"', '']
    for e in emitted:
        lines.append("void %s(Cpu *c);" % emit.fn_name(e))
    for rp, name in extra:
        lines.append("void %s(Cpu *c);" % name)
    for e, _why in stubs:                                  # a stub is still the target of direct calls
        lines.append("void %s(Cpu *c);" % emit.fn_name(e))
    lines.append("")
    lines.extend(body_src)
    for e, why in stubs:
        lines.append('void %s(Cpu *c) { rt_fatal(c, "function %08x was not recompiled: %s"); }' % (emit.fn_name(e), e, why.replace('"', "'").replace("\\", "/")))
    lines.append("")
    covered = sorted(set().union(*[bodies[e].seen for e in bodies if e in set(emitted)])) if emitted else []
    lines.append("void gen_code_%s(void) {" % prefix)
    k = 0
    while k < len(covered):
        j = k
        while j + 1 < len(covered) and covered[j + 1] == covered[j] + 4:
            j += 1
        lines.append("    rt_code_range(0x%08Xu, 0x%08Xu);" % (covered[k], covered[j] + 4))
        k = j + 1
    for ad, ws in sorted(patches.items()):
        lines.append("    { static const uint32_t w[] = {%s}; rt_patch_allow(0x%08Xu, %d, w); }" % (", ".join("0x%08Xu" % x for x in ws), ad, len(ws)))
    lines.append("}")
    lines.append("")
    h = 0xCBF29CE484222325
    for ad in covered:                                   # expected content of every recompiled word: lets the runtime verify an overlay really sits where it was generated for
        for b in img.w(ad).to_bytes(4, "little"):
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    lines.append("int gen_check_%s(Cpu *c) {" % prefix)
    lines.append("    uint64_t h = 0xCBF29CE484222325ull;")
    lines.append("    static const uint32_t r[][2] = {")
    k = 0
    while k < len(covered):
        j = k
        while j + 1 < len(covered) and covered[j + 1] == covered[j] + 4:
            j += 1
        lines.append("        {0x%08Xu, 0x%08Xu}," % (covered[k], covered[j] + 4))
        k = j + 1
    lines.append("    };")
    lines.append("    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++) for (uint32_t a = r[i][0]; a < r[i][1]; a += 4) { uint32_t w; rt_mem_read(c, a, &w, 4); for (int b = 0; b < 4; b++) h = (h ^ ((w >> (8 * b)) & 0xFF)) * 0x100000001B3ull; }")
    lines.append("    return h == 0x%016Xull;" % h)
    lines.append("}")
    lines.append("")
    lines.append("void gen_hooks_%s(void) {" % prefix)
    for addr, name, param, param2, _ in rows:
        lines.append('    rt_hle_bind(0x%08Xu, "%s", 0x%08Xu, 0x%08Xu);' % (addr, name, param, param2))
    lines.append("}")
    lines.append("")
    lines.append("void gen_register_%s(void) {" % prefix)
    for e in sorted(set(emitted) | {s[0] for s in stubs}):
        lines.append('    rt_fn_register(0x%08Xu, %s, "%s", RT_FN_GENERATED);' % (e, emit.fn_name(e), emit.fn_name(e)))
    for rp, name in extra:
        lines.append('    rt_fn_register(0x%08Xu, %s, "%s", RT_FN_GENERATED);' % (rp, name, name))
    lines.append("}")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "w").write("\n".join(lines) + "\n")
    open(out[:-2] + ".funcs", "w").write("\n".join("%08x" % e for e in sorted(emitted)) + "\n")      # for tools/coverage_report.py
    print("%s: %d functions emitted, %d stubs (not recompiled), %d hooked addresses" % (os.path.basename(exe), len(emitted), len(stubs), len(hook_addrs)))
    for e, why in stubs[:10]:
        print("   stub %08x: %s" % (e, why))


if __name__ == "__main__":
    main()
