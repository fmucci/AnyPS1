"""Executable reference semantics for single instructions and short instruction fragments (the "spec").

This is the TEST REFERENCE for the C emitter (`emit.py`), written independently of it: it operates on Python integers, models the
R3000 load delay and branch delay slots dynamically (step by step, with an explicit pending-load register) and implements lwl/lwr/swl/swr
byte by byte from the psx-spx description. It is the seed of the P2-2 oracle but is not a full machine (no BIOS, no I/O, tiny memory window).

Semantics and sources (psx-spx "CPU Instruction Set", "Memory Load Delay", "Multiply/divide", "Unaligned Load/Store"):
  * A load (and mfc0/mfc2/cfc2) writes its GPR after the NEXT executed instruction; that instruction reads the OLD value.
    ASSUMPTION (not stated by psx-spx; follows the 5-stage pipeline and other emulators): if the next instruction writes the same GPR,
    the loaded value is lost.  lwl/lwr targeting the register of a load pending from the previous instruction merge with the PENDING
    value ("no delay required between lwl and lwr").
  * Branches: the instruction after a branch (delay slot) always executes; link registers (jal, jalr, bltzal, bgezal) are written when the
    branch instruction executes, before the slot; bltzal/bgezal write $ra whether or not the branch is taken.
  * mult/div: hi:lo results; divide by zero and 0x80000000 / -1 follow the psx-spx table (no exception).
  * add/addi/sub raise Arithmetic Overflow (exccode 12); syscall = 8; break = 9 (argument = 20-bit code); these stop the fragment.
  * bc2f/bc2t: psx-spx says the COP2 flag reads false: bc2f always branches, bc2t never (assumption from psx-spx text).
  * Misaligned lh/lhu/sh/lw/sw or out-of-window accesses raise AdEL(4)/AdES(5); test fragments avoid them.
"""
from .isa import decode

M32 = 0xFFFFFFFF
MEM_BASE = 0x2000
MEM_SIZE = 256
EXC_OV, EXC_ADEL, EXC_ADES, EXC_SYS, EXC_BP = 12, 4, 5, 8, 9


class Exc(Exception):
    def __init__(self, code, pc, arg=0):
        self.code, self.pc, self.arg = code, pc, arg


class State:
    def __init__(self):
        self.r = [0] * 32
        self.hi = self.lo = 0
        self.mem = bytearray(MEM_SIZE)
        self.cop0 = [0] * 32
        self.cop2d = [0] * 32
        self.cop2c = [0] * 32
        self.cop2cmds = []
        self.pending = None  # (reg, value)
        self.pc = 0
        self.exc = None  # (code, pc, arg)


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def _chk(st, addr, size, store, pc):
    if addr % size or addr < MEM_BASE or addr + size > MEM_BASE + MEM_SIZE:
        raise Exc(EXC_ADES if store else EXC_ADEL, pc, addr)


def load_mem(st, addr, size, pc):
    _chk(st, addr, size, False, pc)
    o = addr - MEM_BASE
    return int.from_bytes(st.mem[o:o + size], "little")


def store_mem(st, addr, size, val, pc):
    _chk(st, addr, size, True, pc)
    o = addr - MEM_BASE
    st.mem[o:o + size] = (val & ((1 << (8 * size)) - 1)).to_bytes(size, "little")


def _byte_addr_ok(addr, pc):
    if addr < MEM_BASE or addr >= MEM_BASE + MEM_SIZE:
        raise Exc(EXC_ADEL, pc, addr)


def divide(st, a, b, signed):
    if signed:
        sa, sb = s32(a), s32(b)
        if sb == 0:
            st.lo, st.hi = (1 if sa < 0 else M32), a
        elif sa == -0x80000000 and sb == -1:
            st.lo, st.hi = 0x80000000, 0
        else:
            q = abs(sa) // abs(sb)
            if (sa < 0) != (sb < 0):
                q = -q
            rem = sa - q * sb
            st.lo, st.hi = q & M32, rem & M32
    else:
        if b == 0:
            st.lo, st.hi = M32, a
        else:
            st.lo, st.hi = a // b, a % b


def execute(st, ins, in_slot=False):
    """Execute one instruction. Returns ('next') or ('branch', taken, target). May raise Exc. Handles the load-delay bookkeeping."""
    n = ins.name
    r = st.r
    pc = ins.pc
    rs, rt, rd = r[ins.rs], r[ins.rt], ins.rd
    old_pending = st.pending
    new_pending = None
    wrote = None  # GPR written during this instruction (not counting the commit of the old pending load)
    result = ("next",)

    def wr(reg, val):
        nonlocal wrote
        if reg != 0:
            r[reg] = val & M32
            wrote = reg
        # writes to $zero are discarded (and do not count as a write that cancels a pending load)

    def ld(reg, val):
        nonlocal new_pending, wrote
        if reg != 0:
            new_pending = (reg, val & M32)
            wrote = reg  # a load targeting the same register as the pending load replaces it

    sh = ins.sa
    if n == "sll": wr(rd, rt << sh)
    elif n == "srl": wr(rd, rt >> sh)
    elif n == "sra": wr(rd, s32(rt) >> sh)
    elif n == "sllv": wr(rd, rt << (rs & 31))
    elif n == "srlv": wr(rd, rt >> (rs & 31))
    elif n == "srav": wr(rd, s32(rt) >> (rs & 31))
    elif n == "addu": wr(rd, rs + rt)
    elif n == "subu": wr(rd, rs - rt)
    elif n == "and": wr(rd, rs & rt)
    elif n == "or": wr(rd, rs | rt)
    elif n == "xor": wr(rd, rs ^ rt)
    elif n == "nor": wr(rd, ~(rs | rt))
    elif n == "slt": wr(rd, 1 if s32(rs) < s32(rt) else 0)
    elif n == "sltu": wr(rd, 1 if rs < rt else 0)
    elif n in ("add", "sub"):
        v = s32(rs) + s32(rt) if n == "add" else s32(rs) - s32(rt)
        if not -0x80000000 <= v <= 0x7FFFFFFF:
            raise Exc(EXC_OV, pc)
        wr(rd, v)
    elif n == "addi":
        v = s32(rs) + ins.simm
        if not -0x80000000 <= v <= 0x7FFFFFFF:
            raise Exc(EXC_OV, pc)
        wr(ins.rt, v)
    elif n == "addiu": wr(ins.rt, rs + ins.simm)
    elif n == "slti": wr(ins.rt, 1 if s32(rs) < ins.simm else 0)
    elif n == "sltiu": wr(ins.rt, 1 if rs < (ins.simm & M32) else 0)
    elif n == "andi": wr(ins.rt, rs & ins.imm)
    elif n == "ori": wr(ins.rt, rs | ins.imm)
    elif n == "xori": wr(ins.rt, rs ^ ins.imm)
    elif n == "lui": wr(ins.rt, ins.imm << 16)
    elif n == "mfhi": wr(rd, st.hi)
    elif n == "mflo": wr(rd, st.lo)
    elif n == "mthi": st.hi = rs
    elif n == "mtlo": st.lo = rs
    elif n == "mult":
        p = s32(rs) * s32(rt); st.lo, st.hi = p & M32, (p >> 32) & M32
    elif n == "multu":
        p = rs * rt; st.lo, st.hi = p & M32, (p >> 32) & M32
    elif n == "div": divide(st, rs, rt, True)
    elif n == "divu": divide(st, rs, rt, False)
    elif n == "syscall": raise Exc(EXC_SYS, pc)
    elif n == "break": raise Exc(EXC_BP, pc, ins.code)
    elif n in ("lw", "lh", "lhu", "lb", "lbu"):
        a = (rs + ins.simm) & M32
        size = {"lw": 4, "lh": 2, "lhu": 2, "lb": 1, "lbu": 1}[n]
        v = load_mem(st, a, size, pc)
        if n == "lh": v = v - 0x10000 if v & 0x8000 else v
        if n == "lb": v = v - 0x100 if v & 0x80 else v
        ld(ins.rt, v)
    elif n in ("lwl", "lwr"):
        a = (rs + ins.simm) & M32
        # merge source: the pending value if the previous instruction is a load into the same register
        base = old_pending[1] if old_pending and old_pending[0] == ins.rt else r[ins.rt]
        k = a & 3
        word_addr = a & ~3
        b = [0] * 4
        for i in range(4):
            _byte_addr_ok(word_addr + i, pc)
            b[i] = st.mem[word_addr + i - MEM_BASE]
        out = [(base >> (8 * i)) & 0xFF for i in range(4)]
        if n == "lwr":  # memory bytes k..3 -> register bytes 0..3-k
            for i in range(k, 4): out[i - k] = b[i]
        else:           # memory bytes 0..k -> register bytes 3-k..3
            for i in range(0, k + 1): out[3 - k + i] = b[i]
        ld(ins.rt, sum(out[i] << (8 * i) for i in range(4)))
    elif n in ("sw", "sh", "sb"):
        a = (rs + ins.simm) & M32
        store_mem(st, a, {"sw": 4, "sh": 2, "sb": 1}[n], rt, pc)
    elif n in ("swl", "swr"):
        a = (rs + ins.simm) & M32
        k = a & 3
        word_addr = a & ~3
        for i in range(4): _byte_addr_ok(word_addr + i, pc)
        rb = [(rt >> (8 * i)) & 0xFF for i in range(4)]
        if n == "swr":  # register bytes 0..3-k -> memory bytes k..3
            for i in range(k, 4): st.mem[word_addr + i - MEM_BASE] = rb[i - k]
        else:           # register bytes 3-k..3 -> memory bytes 0..k
            for i in range(0, k + 1): st.mem[word_addr + i - MEM_BASE] = rb[3 - k + i]
    elif n == "mfc0": ld(ins.rt, st.cop0[rd])
    elif n == "mtc0": st.cop0[rd] = rt
    elif n == "rfe": st.cop0[12] = (st.cop0[12] & ~0xF & M32) | ((st.cop0[12] >> 2) & 0xF)
    elif n == "mfc2": ld(ins.rt, st.cop2d[rd])
    elif n == "cfc2": ld(ins.rt, st.cop2c[rd])
    elif n == "mtc2": st.cop2d[rd] = rt
    elif n == "ctc2": st.cop2c[rd] = rt
    elif n == "lwc2": st.cop2d[ins.rt] = load_mem(st, (rs + ins.simm) & M32, 4, pc)
    elif n == "swc2": store_mem(st, (rs + ins.simm) & M32, 4, st.cop2d[ins.rt], pc)
    elif n == "cop2": st.cop2cmds.append(ins.cop2cmd)
    elif n in ("beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal", "bc2f", "bc2t", "j", "jal", "jr", "jalr"):
        if n == "beq": taken = rs == rt
        elif n == "bne": taken = rs != rt
        elif n == "blez": taken = s32(rs) <= 0
        elif n == "bgtz": taken = s32(rs) > 0
        elif n in ("bltz", "bltzal"): taken = s32(rs) < 0
        elif n in ("bgez", "bgezal"): taken = s32(rs) >= 0
        elif n == "bc2f": taken = True
        elif n == "bc2t": taken = False
        else: taken = True
        if n in ("jr", "jalr"):
            target = rs
        else:
            target = ins.branch_target()
        if n in ("jal", "bltzal", "bgezal"): wr(31, pc + 8)
        if n == "jalr": wr(rd, pc + 8)
        result = ("branch", taken, target)
    else:
        raise ValueError("illegal instruction %r" % ins)

    # commit the load that was pending from the previous instruction, unless this instruction wrote the same register
    if old_pending is not None and old_pending[0] != wrote:
        r[old_pending[0]] = old_pending[1]
    st.pending = new_pending
    return result


def flush(st):
    if st.pending is not None:
        st.r[st.pending[0]] = st.pending[1]
        st.pending = None


def run_fragment(st, words, base, max_steps=400):
    """Run `words` placed at `base` until the PC leaves the fragment or an exception is raised. Returns the exit PC (None on exception)."""
    end = base + 4 * len(words)
    insns = {base + 4 * i: decode(w, base + 4 * i) for i, w in enumerate(words)}
    pc = base
    steps = 0
    try:
        while base <= pc < end:
            steps += 1
            if steps > max_steps:
                raise RuntimeError("fragment did not terminate")
            ins = insns[pc]
            res = execute(st, ins)
            if res[0] == "branch":
                slot_pc = pc + 4
                if slot_pc >= end:
                    raise ValueError("delay slot outside fragment")
                slot = insns[slot_pc]
                try:
                    execute(st, slot, in_slot=True)
                except Exc as e:
                    raise e
                pc = res[2] if res[1] else pc + 8
            else:
                pc += 4
        flush(st)
        st.pc = pc
        return pc
    except Exc as e:
        st.exc = (e.code, e.pc, e.arg)
        return None
