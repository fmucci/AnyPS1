"""Function / entry discovery for PS-X EXEs (queue P2-6; design: docs/decisions/0002-recompiler-c-emission.md section 3.2).

usage: python3 -I tools/recomp/funcs.py EXE [--csv OUT.csv]       (the CSV holds addresses and kinds only, never code)

Recursive descent over the raw bytes, not a linear sweep: a function body is the set of instructions reachable from its entry through
fall-through, branch targets, `j` targets and the instruction after each call's delay slot. Entries come from
  entry        the PS-X EXE header pc
  jal          every `jal`/`bltzal`/`bgezal` target reached by descent
  ptr-data     aligned words in NON-code memory that point at a plausible function start (function-pointer tables)
  ptr-lui      `lui rT,hi ; addiu|ori rT,rT,lo` pairs inside code that build a plausible function start (pointers made at run time)
  root         explicit entries from tools/recomp/roots.csv (function-pointer targets with an unusual start, e.g. a callback registered through OpenEvent)
  leaf-gap     the word after a `jr ra` + delay slot that no discovered body covers (pitfall 14: small leaf functions without a prologue)
"plausible start" = the target decodes cleanly down to a terminator AND (it is already an entry, OR it directly follows `jr ra`+slot,
OR it begins with `addiu sp,sp,-N`). Anything not found here is NOT silently ignored at run time: `rt_call` aborts on an unregistered target.
Jump tables: `jr rX` (X != ra) preceded by `lw rX,off(rY)` and a `lui`; the table is bounded by a preceding `sltiu idx,N` when present
(kind `jumptable`, `bounded=yes`), otherwise by a heuristic (`bounded=no`: entries that are aligned addresses inside the image, at most 256).
Limits (not claims): control flow computed in other ways (jump tables built differently, returns through a modified $ra, tail jumps through
registers) is not followed; a function that never returns makes the descent continue into whatever follows its call.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.recomp.isa import decode  # noqa: E402

JR_RA = 0x03E00008


class Image:
    def __init__(self, path):
        d = open(path, "rb").read()
        assert d[:8] == b"PS-X EXE", path
        self.pc, self.gp, self.base, self.size = struct.unpack("<IIII", d[0x10:0x20])
        self.words = struct.unpack("<%dI" % (self.size // 4), d[0x800:0x800 + self.size])
        self.end = self.base + self.size
        self._dec = {}

    def inside(self, a):
        return self.base <= a < self.end and a % 4 == 0

    def w(self, a):
        return self.words[(a - self.base) // 4]

    def ins(self, a):
        x = self._dec.get(a)
        if x is None:
            x = self._dec[a] = decode(self.w(a), a)
        return x


class Body:
    __slots__ = ("entry", "seen", "calls", "tails", "tables", "bad", "ptr_calls", "hi", "unres", "jal_sites")


def _const_of(img, reg, at, depth=0):
    """Constant value of `reg` just before the instruction at `at`, from the nearest preceding writes in straight-line order (window of 24
    words, branches ignored: only a heuristic to locate jump tables, whose result is validated by the caller)."""
    if reg == 0:
        return 0
    if depth > 4:
        return None
    a = at - 4
    for _ in range(24):
        if a < img.base:
            return None
        i = img.ins(a)
        if i.name in ("lui", "addiu", "ori", "addu", "or", "lw", "lb", "lbu", "lh", "lhu", "sll", "srl", "sra", "subu", "addi", "andi", "xori", "slt", "sltu", "slti", "sltiu", "mfhi", "mflo") and _writes(i) == reg:
            if i.name == "lui":
                return (i.imm << 16) & 0xFFFFFFFF
            if i.name in ("addiu", "addi", "ori"):
                c = _const_of(img, i.rs, a, depth + 1)
                return None if c is None else (c + (i.simm if i.name != "ori" else i.imm)) & 0xFFFFFFFF
            return None
        if i.name not in ("lui", "addiu", "ori", "addu", "or") and _writes(i) == reg:
            return None
        a -= 4
    return None


def _writes(i):
    from tools.recomp.isa import gpr_write
    return gpr_write(i)


def _find_table(img, site, ins):
    """Returns (table_address, targets, bounded) for a `jr rX` jump-table site, or None. Recognised shape:
    `lw rX,off(rY)` with rY = `addu|add rY,rA,rB` where one of rA/rB is a constant built by lui(+addiu/ori), or rY itself a constant."""
    x = ins.rs
    a = site - 4
    lw = None
    for _ in range(14):
        if a < img.base:
            return None
        i = img.ins(a)
        if i.name == "lw" and i.rt == x:
            lw = (a, i)
            break
        if _writes(i) == x:
            return None
        a -= 4
    if lw is None:
        return None
    lw_pc, l = lw
    base = None
    c = _const_of(img, l.rs, lw_pc)
    if c is not None:
        base = c
    else:
        # find the addu that defines rs
        b = lw_pc - 4
        for _ in range(8):
            if b < img.base:
                return None
            j = img.ins(b)
            if _writes(j) == l.rs:
                if j.name in ("addu", "add"):
                    c1, c2 = _const_of(img, j.rs, b), _const_of(img, j.rt, b)
                    base = c1 if c1 is not None else c2
                break
            b -= 4
    if base is None:
        return None
    tab = (base + l.simm) & 0xFFFFFFFF
    if not img.inside(tab):
        return None
    bound = None
    a = site - 4
    for _ in range(30):
        if a < img.base:
            break
        i = img.ins(a)
        if i.name == "sltiu" and 0 < i.simm <= 512:
            bound = i.simm
            break
        a -= 4
    if bound is not None:
        cnt, bounded = bound, True
    else:
        cnt, bounded = 0, False
        while cnt < 256 and img.inside(tab + 4 * cnt) and img.inside(img.w(tab + 4 * cnt)):
            cnt += 1
    if cnt == 0 or tab + 4 * cnt > img.end:
        return None
    tg = [img.w(tab + 4 * k) for k in range(cnt)]
    if not all(img.inside(t) for t in tg):
        return None
    return tab, tg, bounded


def descend(img, entry, entries):
    b = Body()
    b.entry, b.seen, b.calls, b.tails, b.tables, b.bad, b.ptr_calls, b.hi, b.unres = entry, set(), set(), set(), {}, [], set(), entry, []
    b.jal_sites = []
    stack = [entry]
    while stack:
        a = stack.pop()
        zrun = 0
        while True:
            if a in b.seen:
                break
            if not img.inside(a):
                b.bad.append((a, "leaves the image"))
                break
            if img.w(a) == 0:                  # a long run of nops is zero-filled data (a BSS-like gap, memory an overlay is loaded into), not code
                zrun += 1
                if zrun > 64:
                    b.bad.append((a, "zero fill"))
                    break
            else:
                zrun = 0
            i = img.ins(a)
            if i.name == "illegal":
                b.bad.append((a, "illegal word"))
                break
            b.seen.add(a)
            if not i.is_control:
                a += 4
                continue
            if not img.inside(a + 4):
                b.bad.append((a, "delay slot outside image"))
                break
            s = img.ins(a + 4)
            if s.name == "illegal" or s.is_control:
                b.bad.append((a + 4, "illegal or control word in a delay slot"))
                break
            b.seen.add(a + 4)
            nm = i.name
            tgt = i.branch_target()

            def edge(t):
                if t in entries and t != entry:
                    b.tails.add(t)
                else:
                    stack.append(t)
            if nm in ("jal", "bltzal", "bgezal"):
                b.calls.add(tgt)
                if nm == "jal":
                    b.jal_sites.append((a, tgt))
                a += 8
                continue
            if nm == "jalr":
                a += 8
                continue
            if nm == "jr":
                if i.rs != 31:
                    t = _find_table(img, a, i)
                    if not t:
                        b.unres.append(a)
                    if t:
                        b.tables[a] = t
                        for x in t[1]:
                            stack.append(x)      # a jump-table target is always part of this body, even if it is also an entry
                break
            if nm == "j":
                edge(tgt)
                break
            edge(tgt)            # conditional branch
            a += 8
    b.hi = max(b.seen) + 4 if b.seen else entry
    return b


def plausible(img, a, entries, bodies_cache):
    if not img.inside(a):
        return False
    if a in entries:
        return True
    prev_ok = a - 8 >= img.base and img.w(a - 8) == JR_RA
    w0 = img.w(a)
    prologue = (w0 >> 16) == 0x27BD and (w0 & 0x8000) != 0
    if not (prev_ok or prologue):
        return False
    b = bodies_cache.get(a)
    if b is None:
        b = bodies_cache[a] = descend(img, a, entries)
    return not b.bad and any(img.w(x) == JR_RA or img.ins(x).name in ("j", "jr") for x in b.seen)


# the SDK's setjmp (also the BIOS SaveState): `sw ra,0(a0); sw gp,44(a0); sw sp,4(a0); sw s8,8(a0); sw s0,12(a0) ...` -- exact words, no relocation
SETJMP_HEAD = (0xAC9F0000, 0xAC9C002C, 0xAC9D0004, 0xAC9E0008, 0xAC90000C, 0xAC910010)


def setjmp_functions(img):
    out = set()
    for k in range(len(img.words) - len(SETJMP_HEAD)):
        if tuple(img.words[k:k + len(SETJMP_HEAD)]) == SETJMP_HEAD:
            out.add(img.base + 4 * k)
    return out


def bios_setjmp_stubs(img):
    """Addresses of the BIOS call stubs for A(13h) SetJmp: `li t2,A0h; jr t2; li t1,13h` (exact 3-word pattern). A newer SDK calls the BIOS setjmp instead of carrying its own."""
    out = set()
    w = img.words
    for k in range(len(w) - 2):
        if w[k] in (0x240A00A0, 0x340A00A0) and w[k + 1] in (0x01400008, 0x0140F809) and w[k + 2] in (0x24090013, 0x34090013):
            out.add(img.base + 4 * k)
    return out


def resume_points(img, bodies):
    """{function entry: set of addresses}: the instruction after each `jal setjmp` delay slot. A library that registers a jump buffer as its interrupt
    handler (B(19h) SetCustomExitFromException) makes the BIOS 'return' there with r2 = 1 (psx-spx), so such addresses must be enterable."""
    sj = setjmp_functions(img) | bios_setjmp_stubs(img)
    out = {}
    for e, b in bodies.items():
        pts = {a + 8 for a, t in b.jal_sites if t in sj}
        if pts:
            out[e] = pts
    return out


def discover(img, hooked=(), roots=()):
    """`hooked`: guest addresses replaced by HLE hooks. They stay valid entries (calls to them are known) but their bodies are not
    followed, so library code reachable only through them is not pulled in (it can still be found as leaf-gap/ptr candidates)."""
    hooked = set(hooked)
    entries = {img.pc: ("entry", "exe-header")}
    for r in roots:                           # explicit entries (tools/recomp/roots.csv) for callbacks the heuristics cannot see: no prologue, no `jr ra` before them
        entries[r] = ("root", "roots.csv")
    for h in hooked:
        entries[h] = ("hook", "hle")
    cache = {}
    bodies = {}
    for _ in range(12):
        changed = True
        while changed:                       # fixpoint: jal targets
            changed = False
            bodies = {e: descend(img, e, entries) for e in entries if e not in hooked}
            for b in bodies.values():
                for t in b.calls:
                    if img.inside(t) and t not in entries:
                        entries[t] = ("jal", "call from %08x" % b.entry)
                        changed = True
        covered = set()
        for b in bodies.values():
            covered |= b.seen
            for tab, tg, _ in b.tables.values():
                covered |= {tab + 4 * k for k in range(len(tg))}      # table words are data, not function pointers
        new = {}
        cache.clear()
        # leaf-gap: after `jr ra` + slot, not covered
        for k in range(0, len(img.words) - 2):
            a = img.base + 4 * k
            if img.words[k] == JR_RA and img.inside(a + 8) and a + 8 not in covered and a + 8 not in entries:
                if plausible(img, a + 8, entries, cache):
                    new[a + 8] = ("leaf-gap", "after jr ra at %08x" % a)
        # ptr-data: words outside code
        for k, v in enumerate(img.words):
            a = img.base + 4 * k
            if a in covered or v % 4 or v in entries or v in new:
                continue
            if plausible(img, v, entries, cache):
                new[v] = ("ptr-data", "word at %08x" % a)
        # ptr-lui: lui/addiu|ori pairs inside code
        for a in sorted(covered):
            i = img.ins(a)
            if i.name == "lui" and img.inside(a + 4) and a + 4 in covered:
                j = img.ins(a + 4)
                if j.name in ("addiu", "ori") and j.rs == i.rt and j.rt == i.rt:
                    v = ((i.imm << 16) + (j.simm if j.name == "addiu" else j.imm)) & 0xFFFFFFFF
                    if v % 4 == 0 and v not in entries and v not in new and plausible(img, v, entries, cache):
                        new[v] = ("ptr-lui", "pair at %08x" % a)
        if not new:
            break
        entries.update(new)
    bodies = {e: descend(img, e, entries) for e in entries if e not in hooked}
    return entries, bodies


def main():
    args = sys.argv[1:]
    csv = None
    if "--csv" in args:
        k = args.index("--csv")
        csv = args[k + 1]
        del args[k:k + 2]
    img = Image(args[0])
    entries, bodies = discover(img)
    kinds = {}
    for e, (k, _) in entries.items():
        kinds[k] = kinds.get(k, 0) + 1
    bad = [b for b in bodies.values() if b.bad]
    tabs = [(s, t) for b in bodies.values() for s, t in b.tables.items()]
    covered = set()
    for b in bodies.values():
        covered |= b.seen
    unres = sorted(a for b in bodies.values() for a in b.unres)
    print("%d `jr rX` sites without a recovered table (they become tail `rt_call`, which aborts on an unregistered target)" % len(unres))
    print("%s: %d functions %s; %d with decode problems; %d jump tables (%d unbounded); %d of %d words reached as code" % (
        os.path.basename(args[0]), len(entries), dict(sorted(kinds.items())), len(bad), len(tabs),
        sum(1 for _, t in tabs if not t[2]), len(covered), len(img.words)))
    for b in sorted(bad, key=lambda b: b.entry)[:8]:
        print("   problem in %08x: %s" % (b.entry, b.bad[0]))
    if csv:
        with open(csv, "w") as f:
            f.write("address,kind,source\n")
            for e in sorted(entries):
                f.write("0x%08x,function,%s:%s\n" % (e, entries[e][0], entries[e][1]))
            for s, t in sorted(tabs):
                f.write("0x%08x,jumptable,table=0x%08x;entries=%d;bounded=%s\n" % (s, t[0], len(t[1]), "yes" if t[2] else "no"))


if __name__ == "__main__":
    main()


def unresolved_code_pointers(img, ents, bodies, tables=False, min_instr=8):
    """Code addresses that are built with `lui`+`addiu|ori` inside discovered code or stored as data words, point at code that was not emitted and look like a function (relaxed rule:
    no prologue needed; zero-base memory accesses mean a jump table decoded as code). Returns [(target, where, instructions)]. Used by tools/recomp/unresolved_pointers.py and gen.py --auto-roots."""
    covered = set()
    for b in bodies.values():
        covered |= b.seen
    mem_ops = ("lb", "lbu", "lh", "lhu", "lw", "sb", "sh", "sw")
    out = []

    def at_boundary(v):                          # the previous function ended just before v (jr ra and its delay slot, optionally followed by padding nops)
        return img.inside(v - 12) and (img.w(v - 8) == JR_RA or (img.w(v - 4) == 0 and img.w(v - 12) == JR_RA))

    def consider(v, where, data_word=False):
        if v % 4 or not img.inside(v) or v in ents:
            return
        if v in covered and not (tables and (data_word or at_boundary(v))):          # with tables=True a data word may name an address in the middle of an emitted body (e.g. a constructor after an unconditional branch)
            return
        first = img.ins(v)
        if first.name in mem_ops and first.rs == 0:
            return
        body = descend(img, v, ents)
        if not body.bad and len(body.seen) >= min_instr and any(img.w(x) == JR_RA or img.ins(x).name in ("j", "jr") for x in body.seen):
            out.append((v, where, len(body.seen)))
    for a in sorted(covered):
        i = img.ins(a)
        if i.name == "lui" and img.inside(a + 4) and a + 4 in covered:
            j = img.ins(a + 4)
            if j.name in ("addiu", "ori") and j.rs == i.rt and j.rt == i.rt:
                consider(((i.imm << 16) + (j.simm if j.name == "addiu" else j.imm)) & 0xFFFFFFFF, a)
        if i.name == "lui" and img.inside(a + 8) and a + 4 in covered and a + 8 in covered and img.ins(a + 4).name == "jal":      # lui; jal f; addiu (the delay slot completes the address passed to f)
            j = img.ins(a + 8)
            if j.name in ("addiu", "ori") and j.rs == i.rt and j.rt == i.rt:
                consider(((i.imm << 16) + (j.simm if j.name == "addiu" else j.imm)) & 0xFFFFFFFF, a)
    for k, w in enumerate(img.words):
        a = img.base + 4 * k
        if a not in covered:
            consider(w, a, data_word=True)
    if tables:                                   # runs of >= 3 data words that all point into emitted code = a jump table the pattern matcher did not recognise: its targets are entries
        run = []
        for k, w in enumerate(list(img.words) + [0]):
            a = img.base + 4 * k
            if a not in covered and w % 4 == 0 and img.inside(w) and (w in covered or w in ents):
                run.append((w, a))
                continue
            if len(run) >= 3:
                out += [(t, at, 0) for t, at in run if t not in ents]
            run = []
    return out
