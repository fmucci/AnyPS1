"""MIPS-I (R3000A) instruction decoder for the PlayStation, including the COP0/COP2 forms used by games.

Pure decoding: no semantics here (those live in `spec.py` and `emit.py`, written independently of each other).
`decode(word, pc)` returns an `Insn`; words that are not valid R3000A instructions (or use coprocessors the PS1 does not have)
decode to name == "illegal" and must never be emitted as code.

Sources: MIPS-I encoding (SPECIAL / REGIMM / I / J), psx-spx "CPU Instruction Set" (load/store, COP0, COP2 command word bit 25).
Decisions recorded in docs/decisions/0002-recompiler-c-emission.md.
"""

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break",
    0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
    0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
    0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
}
OPS = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x32: "lwc2", 0x3A: "swc2",
}
LOADS = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr"}
STORES = {"sb", "sh", "swl", "sw", "swr"}
# instructions whose destination GPR is written one instruction late (load delay slot)
DELAYED_GPR_WRITE = LOADS | {"mfc0", "mfc2", "cfc2"}
CONTROL = {"j", "jal", "jr", "jalr", "beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal", "bc2f", "bc2t"}
CAN_EXCEPT = {"add", "addi", "sub", "syscall", "break"}


class Insn:
    __slots__ = ("name", "word", "pc", "rs", "rt", "rd", "sa", "imm", "simm", "target", "code")

    def __init__(self, name, word, pc):
        self.name, self.word, self.pc = name, word, pc
        self.rs, self.rt, self.rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = word & 0x3FFFFFF
        self.code = (word >> 6) & 0xFFFFF  # syscall/break code field; also the COP2 25-bit command is word & 0x1FFFFFF

    @property
    def cop2cmd(self):
        return self.word & 0x1FFFFFF

    @property
    def is_control(self):
        return self.name in CONTROL

    def branch_target(self):
        """Absolute target of a PC-relative branch or `j/jal`; None for register jumps."""
        if self.name in ("j", "jal"):
            return ((self.pc + 4) & 0xF0000000) | (self.target << 2)
        if self.name in CONTROL and self.name not in ("jr", "jalr"):
            return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF
        return None

    def __repr__(self):
        return "<%s %08x @%08x>" % (self.name, self.word, self.pc)


def decode(word, pc=0):
    op = word >> 26
    rs, rt = (word >> 21) & 31, (word >> 16) & 31
    if op == 0:
        name = SPECIAL.get(word & 0x3F, "illegal")
        # shifts by immediate require rs == 0 on real encodings; mfhi/mflo etc. are not checked strictly
    elif op == 1:
        # rt bit 0 selects ge/lt; link only when (rt & 0x1E) == 0x10 (DuckStation cpu_core.cpp `BEGIN_INSTRUCTION(b)`, finding 0020)
        name = ("bgez" if rt & 1 else "bltz") if (rt & 0x1E) != 0x10 else ("bgezal" if rt & 1 else "bltzal")
    elif op == 0x10:
        if rs == 0:
            name = "mfc0"
        elif rs == 4:
            name = "mtc0"
        elif rs >= 0x10 and (word & 0x3F) == 0x10:
            name = "rfe"
        else:
            name = "illegal"
    elif op == 0x12:
        if word & (1 << 25):
            name = "cop2"
        else:
            name = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(rs, "illegal")
            if rs == 8:
                name = "bc2t" if rt & 1 else "bc2f"
    else:
        name = OPS.get(op, "illegal")
    return Insn(name, word, pc)


def gpr_reads(i):
    """GPRs read by the instruction (excluding $zero)."""
    n = i.name
    if n in ("sll", "srl", "sra"):
        r = {i.rt}
    elif n in ("sllv", "srlv", "srav", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu", "mult", "multu", "div", "divu", "beq", "bne"):
        r = {i.rs, i.rt}
    elif n in ("jr", "jalr", "mthi", "mtlo", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal", "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lb", "lh", "lw", "lbu", "lhu", "lwc2", "swc2"):
        r = {i.rs}
    elif n in ("lwl", "lwr"):
        r = {i.rs, i.rt}  # rt is the merge source
    elif n in STORES:
        r = {i.rs, i.rt}
    elif n in ("mtc0", "mtc2", "ctc2"):
        r = {i.rt}
    else:
        r = set()
    r.discard(0)
    return r


def gpr_write(i):
    """GPR written by the instruction (None for none or $zero). For DELAYED_GPR_WRITE names the write happens one instruction late."""
    n = i.name
    if n in ("sll", "srl", "sra", "sllv", "srlv", "srav", "mfhi", "mflo", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"):
        d = i.rd
    elif n == "jalr":
        d = i.rd
    elif n in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui") or n in LOADS or n in ("mfc0", "mfc2", "cfc2"):
        d = i.rt
    elif n in ("jal", "bltzal", "bgezal"):
        d = 31
    else:
        d = None
    return d or None
