"""C emitter for MIPS-I instruction fragments (queue P2-7; design: docs/decisions/0002-recompiler-c-emission.md sections 3.2-3.4).

`emit_fragment(words, base, name)` turns a straight run of instructions at guest address `base` into one C function
`void name(Cpu *c)` operating on `runtime/cpu.h`. Control flow inside the fragment becomes `goto`; leaving the fragment sets `c->pc`
and returns. (Whole-function emission with direct calls/returns is built on this in P2-3; the instruction-level semantics here are final.)

Load delay (decision 0002 s3.4), implemented statically:
  * a load / mfc0 / mfc2 / cfc2 computes its value into a temporary `ldN`; the GPR is assigned right after the NEXT executed instruction,
    which therefore reads the old value; if that instruction writes the same GPR the pending value is dropped;
  * lwl/lwr after a load into the same GPR merge with the pending temporary;
  * a branch instruction counts as the "next instruction" of a load right before it (it reads old values, the commit happens before the slot);
  * a load in a delay slot is committed immediately after the slot when committing early cannot be observed on any known successor
    path (see `bad_successor`: the successor reads the register, or overwrites it with another delayed write and the instruction after
    it reads it); otherwise `HazardError` is raised (unsupported, never guessed). Successors outside the fragment and register jumps are unknown:
    the commit is done immediately and the site is listed in `report["unchecked_slot_loads"]`;
  * a load directly followed by a branch-target label that `bad_successor` flags: `HazardError`.
Delay-slot instructions may be exception-capable (add/addi/sub/syscall/break occur in the game's delay slots): a raise reports the PC of
the faulting slot instruction (hardware reports the branch in EPC with the BD bit; irrelevant because the runtime aborts on any exception).
A branch target that lands ON a delay-slot instruction (legal MIPS: the instruction is also executed on its own) jumps to a stub placed
after the function body that runs the instruction and continues at the instruction after the slot.
Not supported (raise `Unsupported`): a control transfer in a delay slot, a misaligned target, `illegal` words.
Writes to $zero are dropped (their side effects, e.g. memory faults and overflow checks, are kept).
"""
from .isa import DELAYED_GPR_WRITE, decode, gpr_reads, gpr_write


YIELD_OPS = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bc2f", "bc2t", "j"}


class HazardError(Exception):
    pass


class Unsupported(Exception):
    pass


def R(i):
    return "0u" if i == 0 else "c->r[%d]" % i


def U(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def W(i, expr):
    return "" if i == 0 else "c->r[%d] = %s;" % (i, expr)


class _Out:
    __slots__ = ("stmts", "load")

    def __init__(self, stmts, load=None):
        self.stmts, self.load = stmts, load  # load = (reg, tmpname) when the GPR write is delayed


def _addr(i):
    return "(%s + %s)" % (R(i.rs), U(i.simm))


def gen(ins, tmp_alloc, pend):
    """C statements for one non-control instruction. `pend` = (reg, tmp) of the load pending from the previous instruction, or None."""
    n, rs, rt, rd, sa = ins.name, ins.rs, ins.rt, ins.rd, ins.sa
    A, B = R(rs), R(rt)
    s = []
    load = None

    def delayed(reg, expr):
        nonlocal load
        if reg == 0:
            s.append("(void)(%s);" % expr)
        else:
            t = tmp_alloc()
            s.append("%s = %s;" % (t, expr))
            load = (reg, t)

    if n == "sll": s.append(W(rd, "%s << %d" % (B, sa)))
    elif n == "srl": s.append(W(rd, "%s >> %d" % (B, sa)))
    elif n == "sra": s.append(W(rd, "rt_sra(%s, %d)" % (B, sa)))
    elif n == "sllv": s.append(W(rd, "%s << (%s & 31u)" % (B, A)))
    elif n == "srlv": s.append(W(rd, "%s >> (%s & 31u)" % (B, A)))
    elif n == "srav": s.append(W(rd, "rt_sra(%s, %s & 31u)" % (B, A)))
    elif n == "addu": s.append(W(rd, "%s + %s" % (A, B)))
    elif n == "subu": s.append(W(rd, "%s - %s" % (A, B)))
    elif n == "and": s.append(W(rd, "%s & %s" % (A, B)))
    elif n == "or": s.append(W(rd, "%s | %s" % (A, B)))
    elif n == "xor": s.append(W(rd, "%s ^ %s" % (A, B)))
    elif n == "nor": s.append(W(rd, "~(%s | %s)" % (A, B)))
    elif n == "slt": s.append(W(rd, "(uint32_t)((int32_t)%s < (int32_t)%s)" % (A, B)))
    elif n == "sltu": s.append(W(rd, "(uint32_t)(%s < %s)" % (A, B)))
    elif n == "add":
        s.append("{ uint32_t a = %s, b = %s, r = a + b; if (((a ^ r) & (b ^ r)) >> 31) { rt_exception(c, %s, 12u, 0u); return; } %s }" % (A, B, U(ins.pc), W(rd, "r")))
    elif n == "sub":
        s.append("{ uint32_t a = %s, b = %s, r = a - b; if (((a ^ b) & (a ^ r)) >> 31) { rt_exception(c, %s, 12u, 0u); return; } %s }" % (A, B, U(ins.pc), W(rd, "r")))
    elif n == "addi":
        s.append("{ uint32_t a = %s, b = %s, r = a + b; if (((a ^ r) & (b ^ r)) >> 31) { rt_exception(c, %s, 12u, 0u); return; } %s }" % (A, U(ins.simm), U(ins.pc), W(rt, "r")))
    elif n == "addiu": s.append(W(rt, "%s + %s" % (A, U(ins.simm))))
    elif n == "slti": s.append(W(rt, "(uint32_t)((int32_t)%s < (int32_t)%s)" % (A, U(ins.simm))))
    elif n == "sltiu": s.append(W(rt, "(uint32_t)(%s < %s)" % (A, U(ins.simm))))
    elif n == "andi": s.append(W(rt, "%s & %s" % (A, U(ins.imm))))
    elif n == "ori": s.append(W(rt, "%s | %s" % (A, U(ins.imm))))
    elif n == "xori": s.append(W(rt, "%s ^ %s" % (A, U(ins.imm))))
    elif n == "lui": s.append(W(rt, U(ins.imm << 16)))
    elif n == "mfhi": s.append(W(rd, "c->hi"))
    elif n == "mflo": s.append(W(rd, "c->lo"))
    elif n == "mthi": s.append("c->hi = %s;" % A)
    elif n == "mtlo": s.append("c->lo = %s;" % A)
    elif n == "mult":
        s.append("{ int64_t p = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); }" % (A, B))
    elif n == "multu":
        s.append("{ uint64_t p = (uint64_t)%s * (uint64_t)%s; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }" % (A, B))
    elif n == "div": s.append("rt_div(c, %s, %s);" % (A, B))
    elif n == "divu": s.append("rt_divu(c, %s, %s);" % (A, B))
    elif n == "syscall": s.append("{ rt_exception(c, %s, 8u, 0u); if (c->exc) return; }" % U(ins.pc))
    elif n == "break": s.append("{ rt_exception(c, %s, 9u, %s); return; }" % (U(ins.pc), U(ins.code)))
    elif n == "lw": delayed(rt, "rt_lw(c, %s)" % _addr(ins))
    elif n == "lh": delayed(rt, "rt_lh(c, %s)" % _addr(ins))
    elif n == "lhu": delayed(rt, "rt_lhu(c, %s)" % _addr(ins))
    elif n == "lb": delayed(rt, "rt_lb(c, %s)" % _addr(ins))
    elif n == "lbu": delayed(rt, "rt_lbu(c, %s)" % _addr(ins))
    elif n in ("lwl", "lwr"):
        base = pend[1] if pend and pend[0] == rt else B  # forwarding between consecutive lwl/lwr
        if rt == 0:
            s.append("(void)rt_lw(c, %s & ~3u);" % _addr(ins))
        else:
            t = tmp_alloc()
            if n == "lwl":
                expr = "(%s & (0x00FFFFFFu >> (8u * (a & 3u)))) | (w << (24u - 8u * (a & 3u)))" % base
            else:
                expr = "(%s & ~(0xFFFFFFFFu >> (8u * (a & 3u)))) | (w >> (8u * (a & 3u)))" % base
            s.append("{ uint32_t a = %s; uint32_t w = rt_lw(c, a & ~3u); %s = %s; }" % (_addr(ins), t, expr))
            load = (rt, t)
    elif n == "sw": s.append("rt_sw(c, %s, %s);" % (_addr(ins), B))
    elif n == "sh": s.append("rt_sh(c, %s, %s);" % (_addr(ins), B))
    elif n == "sb": s.append("rt_sb(c, %s, %s);" % (_addr(ins), B))
    elif n == "swl":
        s.append("{ uint32_t a = %s, v = %s; uint32_t m = rt_lw(c, a & ~3u); uint32_t sh = 24u - 8u * (a & 3u); rt_sw(c, a & ~3u, (m & ~(0xFFFFFFFFu >> sh)) | (v >> sh)); }" % (_addr(ins), B))
    elif n == "swr":
        s.append("{ uint32_t a = %s, v = %s; uint32_t m = rt_lw(c, a & ~3u); uint32_t sh = 8u * (a & 3u); rt_sw(c, a & ~3u, (m & ~(0xFFFFFFFFu << sh)) | (v << sh)); }" % (_addr(ins), B))
    elif n == "mfc0": delayed(rt, "cop0_mfc0(c, %d)" % rd)
    elif n == "mtc0": s.append("cop0_mtc0(c, %d, %s);" % (rd, B))
    elif n == "rfe": s.append("cop0_rfe(c);")
    elif n == "mfc2": delayed(rt, "gte_mfc2(c, %d)" % rd)
    elif n == "cfc2": delayed(rt, "gte_cfc2(c, %d)" % rd)
    elif n == "mtc2": s.append("gte_mtc2(c, %d, %s);" % (rd, B))
    elif n == "ctc2": s.append("gte_ctc2(c, %d, %s);" % (rd, B))
    elif n == "lwc2": s.append("gte_mtc2(c, %d, rt_lw(c, %s));" % (rt, _addr(ins)))
    elif n == "swc2": s.append("rt_sw(c, %s, gte_mfc2(c, %d));" % (_addr(ins), rt))
    elif n == "cop2": s.append("gte_cmd(c, 0x%07Xu);" % ins.cop2cmd)
    else:
        raise Unsupported("cannot emit %r" % ins)
    return _Out([x for x in s if x], load)


PREFIX = ""          # set by gen.py (per executable) so that several recompiled executables can be linked into one program


def fn_name(addr):
    return "f_%s%08x" % (PREFIX, addr & 0xFFFFFFFF)


def emit_fragment(words, base, name="frag", fn=None):
    """Returns (C source, report). `report["unchecked_slot_loads"]` lists load-in-delay-slot sites whose successor is not known.

    Whole-function mode (`fn` = {"known": set of function entry addresses, "tables": {pc_of_jr: [target addresses]}}, queue P2-3):
      * `jal`/`jalr`/`bltzal`/`bgezal` call (direct `f_XXXXXXXX(c)` if the target is in `known`, else `rt_call`, which aborts if nothing is
        registered there) and execution continues after the delay slot;
      * `jr ra` returns (the guest $ra is not consulted: assumption R1, checked by the traces of P2-3);
      * `jr rX` (X != ra) is a switch over the table's targets when `tables` has the site, else a tail `rt_call`;
      * a `j` or taken branch that leaves the body is a tail call; running off the end calls the next address (`rt_call`);
      * `c->pc` is set before every instruction that touches memory, COP0 or the GTE so that fatal messages name the site."""
    n = len(words)
    end = base + 4 * n
    insns = [decode(w, base + 4 * i) for i, w in enumerate(words)]
    idx = lambda pc: (pc - base) // 4
    report = {"unchecked_slot_loads": []}
    for i in insns:
        if i.name == "illegal":
            raise Unsupported("illegal instruction %r" % i)
    # delay-slot positions and branch targets inside the fragment
    slot_idx = set()
    for k, i in enumerate(insns):
        if i.is_control:
            if k + 1 >= n:
                raise Unsupported("delay slot of %r is outside the fragment" % i)
            s = insns[k + 1]
            if s.is_control:
                raise Unsupported("control transfer in a delay slot: %r" % s)
            slot_idx.add(k + 1)
    leaders = {0}
    entry_idx = idx(fn["entry"]) if fn is not None and fn.get("entry") is not None else 0
    leaders.add(entry_idx)
    slot_targets = set()
    if entry_idx in slot_idx:                      # a function entry that is the delay slot of a branch (hand-written assembly with shared tails): enter through the slot label
        leaders.discard(entry_idx)
        slot_targets.add(entry_idx)
        if entry_idx + 1 < n:
            leaders.add(entry_idx + 1)
    for i in insns:
        t = i.branch_target() if i.is_control else None
        if t is not None and base <= t < end:
            if t % 4:
                raise Unsupported("misaligned target in %r" % i)
            if idx(t) in slot_idx:
                slot_targets.add(idx(t))
                if idx(t) + 1 < n:
                    leaders.add(idx(t) + 1)
            else:
                leaders.add(idx(t))

    if fn is not None:
        for tg in fn["tables"].values():
            for t in tg:
                if base <= t < end:
                    if t % 4:
                        raise Unsupported("misaligned jump-table target %08x" % t)
                    if idx(t) in slot_idx:
                        slot_targets.add(idx(t))
                        if idx(t) + 1 < n:
                            leaders.add(idx(t) + 1)
                    else:
                        leaders.add(idx(t))

    lines, decls = [], []
    need_ra0 = [False]
    tmp_n = [0]

    def tmp():
        t = "ld%d" % tmp_n[0]
        tmp_n[0] += 1
        decls.append(t)
        return t

    def bad_successor(xi, d):
        """True if committing a load of register d EARLY (before the instruction at index xi) could be observed on the straight-line
        path starting there. On the real machine the pending value is dropped when an instruction writes d, so the old value of d stays
        visible until the end of a chain of delayed writes to d (load/mfc0/mfc2/cfc2 into d, one after the other). Observable if any
        instruction in that chain, or the first instruction after it, reads d. Anything that cannot be followed (end of fragment, a
        control transfer inside the chain) is treated as observable."""
        i = xi
        while True:
            if i >= n:
                return True
            x = insns[i]
            if d in gpr_reads(x):
                return True
            if gpr_write(x) == d and x.name in DELAYED_GPR_WRITE:
                i += 1
                if i < n and insns[i].is_control:
                    return True
                continue
            return False  # does not touch d, or overwrites it immediately: nothing observable any more

    def at_known(pc):
        return insns[idx(pc)] if base <= pc < end and (pc - base) % 4 == 0 else None

    pend = None  # (reg, tmp) pending from the previous executed instruction
    k = 0
    used_labels = set()
    if entry_idx != 0:
        lines.append(("goto S_%d;" if entry_idx in slot_targets else "goto L_%d;") % entry_idx)   # the body extends below the entry address (code placed before the entry, reached by a jump)
        if entry_idx not in slot_targets:
            used_labels.add(entry_idx)
    while k < n:
        i = insns[k]
        if k in leaders:
            if pend is not None:
                if bad_successor(k, pend[0]):
                    raise HazardError("load %s then instruction at 0x%08x (a branch target) uses it" % (pend[1], i.pc))
                lines.append(W(pend[0], pend[1]))
                pend = None
            lines.append("L_%d:;" % k)
        if not i.is_control:
            if fn is not None and i.pc in fn.get("patches", {}):
                # self-modifying code (finding 0023): the word at this address is rewritten at run time between known variants; the instruction is
                # selected by the CURRENT memory content. Only side-effect-free ALU variants that write the same register are supported.
                variants = [decode(w, i.pc) for w in fn["patches"][i.pc]]
                if any(v.is_control or v.name in DELAYED_GPR_WRITE or v.name in ("syscall", "break") for v in variants) or len({gpr_write(v) for v in variants}) != 1:
                    raise Unsupported("patched instruction at 0x%08x: unsupported variants" % i.pc)
                body = []
                for w, v in zip(fn["patches"][i.pc], variants):
                    vo = gen(v, tmp, pend)
                    body.append("case %s: %s break;" % (U(w), " ".join(vo.stmts)))
                o = _Out(["switch (rt_lw(c, %s)) { %s default: rt_fatal(c, \"unproven code word at %%08x\", %s); }" % (U(i.pc), " ".join(body), U(i.pc))])
                lines.append("c->pc = %s;" % U(i.pc))
            else:
                o = gen(i, tmp, pend)
            if fn is not None and any(("rt_l" in x or "rt_s" in x or "cop0_" in x or "gte_" in x) for x in o.stmts):
                lines.append("c->pc = %s;" % U(i.pc))
            lines.extend("%s" % x for x in o.stmts)
            written = gpr_write(i)
            if pend is not None:
                if written != pend[0]:
                    lines.append(W(pend[0], pend[1]))
                # else: dropped, this instruction (or load) writes the same register
            pend = o.load
            k += 1
            continue
        # ---- control transfer at k, delay slot at k+1 ----
        b, s = i, insns[k + 1]
        lines.append("{")
        nm = b.name
        cond = None
        if nm == "beq": cond = "%s == %s" % (R(b.rs), R(b.rt))
        elif nm == "bne": cond = "%s != %s" % (R(b.rs), R(b.rt))
        elif nm == "blez": cond = "(int32_t)%s <= 0" % R(b.rs)
        elif nm == "bgtz": cond = "(int32_t)%s > 0" % R(b.rs)
        elif nm in ("bltz", "bltzal"): cond = "(int32_t)%s < 0" % R(b.rs)
        elif nm in ("bgez", "bgezal"): cond = "(int32_t)%s >= 0" % R(b.rs)
        elif nm == "bc2f": cond = "1"   # psx-spx: the COP2 flag reads false, so bc2f always branches
        elif nm == "bc2t": cond = "0"
        if cond is not None:
            lines.append("int br = (%s);" % cond)
        if nm in ("jr", "jalr"):
            lines.append("uint32_t t = %s;" % R(b.rs))
        link = None
        if nm in ("jal", "bltzal", "bgezal"):
            link = (31, U(b.pc + 8))
        if nm == "jalr":
            link = (b.rd, U(b.pc + 8))
        if link and link[0] != 0:
            lines.append(W(link[0], link[1]))
        lw = link[0] if link and link[0] != 0 else None
        if pend is not None:
            if lw != pend[0]:
                lines.append(W(pend[0], pend[1]))
            pend = None
        # delay slot
        o = gen(s, tmp, None)
        if fn is not None and any(("rt_l" in x or "rt_s" in x or "cop0_" in x or "gte_" in x) for x in o.stmts):
            lines.append("c->pc = %s;" % U(s.pc))
        lines.extend(o.stmts)
        tgt = b.branch_target()
        if o.load:
            d, t = o.load
            succ_known = True
            succs = []
            if nm in ("jr", "jalr"):
                succ_known = False
            else:
                if cond != "0":
                    succs.append(tgt)
                if nm not in ("j", "jal", "bc2f") and not (cond == "1"):
                    succs.append(b.pc + 8)
            for a in succs:
                x = at_known(a)
                if x is None:
                    succ_known = False
                elif bad_successor(idx(a), d):
                    raise HazardError("load in the delay slot of 0x%08x: successor 0x%08x uses r%d" % (b.pc, a, d))
            if not succ_known:
                report["unchecked_slot_loads"].append(s.pc)
            lines.append(W(d, t))
        # transfer
        def call(addr):
            if fn is not None and (addr & 0x1FFFFFFF) in fn["known"]:
                return "%s(c);" % fn_name(addr)
            return "rt_call(c, %s);" % U(addr)

        def go(addr, count=True):
            # cooperative time (decisions/0002 s3.6): a TAKEN direct branch / `j` to a lower-or-equal address counts one tick of the
            # yield counter; the oracle counts exactly the same events (tools/recomp/oracle.py)
            y = "if (--c->yield <= 0) rt_yield(c); " if (fn is not None and count and nm in YIELD_OPS and addr <= b.pc) else ""
            if base <= addr < end:
                used_labels.add(idx(addr))
                return ("{ %sgoto S_%d; }" if idx(addr) in slot_targets else "{ %sgoto L_%d; }") % (y, idx(addr))
            if fn is not None:
                return "{ %s%s return; }" % (y, call(addr))
            return "{ c->pc = %s; goto L_exit; }" % U(addr)
        if fn is not None:
            if nm == "jr" and b.rs == 31:
                lines.append("return;")
            elif nm == "jr":
                tg = fn["tables"].get(b.pc)
                if tg:
                    lines.append("c->pc = %s;" % U(b.pc))
                    lines.append("switch (t) {")
                    for a in sorted(set(tg)):
                        lines.append("case %s: %s" % (U(a), go(a, False)))
                    lines.append("default: rt_fatal(c, \"jump table of jr at %08x has no entry %08x\", " + U(b.pc) + ", t);")
                    lines.append("}")
                else:
                    need_ra0[0] = True
                    lines.append("if (t == ra0) return;                      /* jr through a saved return address (move s3,ra ... jr s3): this is the function's return, not a tail call */")
                    lines.append("c->pc = %s; rt_call(c, t); return;" % U(b.pc))
            elif nm == "jalr":
                lines.append("c->pc = %s; rt_call(c, t);" % U(b.pc))
            elif nm == "jal":
                lines.append(call(tgt))
            elif nm in ("bltzal", "bgezal"):
                lines.append("if (br) { %s }" % call(tgt))
            elif nm == "j":
                lines.append(go(tgt))
            else:
                lines.append("if (br) %s" % go(tgt))
        elif nm in ("jr", "jalr"):
            lines.append("c->pc = t; goto L_exit;")
        elif nm in ("j", "jal"):
            lines.append(go(tgt))
        else:
            lines.append("if (br) %s" % go(tgt))
        lines.append("}")
        k += 2
    if pend is not None:
        lines.append(W(pend[0], pend[1]))
    if fn is not None:
        lines.append("c->pc = %s; rt_call(c, %s); return;" % (U(end), U(end)))
        lines.append("L_exit:;")
        lines.append("return;")
    else:
        lines.append("c->pc = %s;" % U(end))
        lines.append("L_exit:;")
        lines.append("return;")
    for sidx in sorted(slot_targets):
        lines.append("S_%d:;" % sidx)
        o = gen(insns[sidx], tmp, None)
        lines.extend(o.stmts)
        if o.load:
            d, t = o.load
            if sidx + 1 < n and bad_successor(sidx + 1, d):
                raise HazardError("slot instruction 0x%08x used as a branch target loads r%d, next instruction uses it" % (insns[sidx].pc, d))
            lines.append(W(d, t))
        if sidx + 1 < n:
            lines.append("goto L_%d;" % (sidx + 1))
        elif fn is not None:
            lines.append("c->pc = %s; rt_call(c, %s); return;" % (U(end), U(end)))
        else:
            lines.append("c->pc = %s; goto L_exit;" % U(end))
    src = ["void %s(Cpu *c) {" % name]
    if fn is not None:
        src.append("    RT_ENTER(%s);" % U(fn.get("entry") if fn.get("entry") is not None else base))
    if need_ra0[0]:
        src.append("    const uint32_t ra0 = c->r[31];                          /* the caller's return address at entry (see the unresolved jr below) */")
    if decls:
        src.append("    uint32_t %s;" % ", ".join(decls))
    src.extend("    " + x for x in lines)
    src.append("}")
    return "\n".join(src), report
