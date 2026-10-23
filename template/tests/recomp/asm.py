"""Tiny MIPS-I assembler for the tests (word builders only; independent of tools/recomp/isa.py decoding tables)."""
FUNCT = {"sll": 0x00, "srl": 0x02, "sra": 0x03, "sllv": 0x04, "srlv": 0x06, "srav": 0x07, "jr": 0x08, "jalr": 0x09,
         "syscall": 0x0C, "break": 0x0D, "mfhi": 0x10, "mthi": 0x11, "mflo": 0x12, "mtlo": 0x13, "mult": 0x18, "multu": 0x19,
         "div": 0x1A, "divu": 0x1B, "add": 0x20, "addu": 0x21, "sub": 0x22, "subu": 0x23, "and": 0x24, "or": 0x25,
         "xor": 0x26, "nor": 0x27, "slt": 0x2A, "sltu": 0x2B}
OPC = {"j": 2, "jal": 3, "beq": 4, "bne": 5, "blez": 6, "bgtz": 7, "addi": 8, "addiu": 9, "slti": 10, "sltiu": 11,
       "andi": 12, "ori": 13, "xori": 14, "lui": 15, "lb": 0x20, "lh": 0x21, "lwl": 0x22, "lw": 0x23, "lbu": 0x24,
       "lhu": 0x25, "lwr": 0x26, "sb": 0x28, "sh": 0x29, "swl": 0x2A, "sw": 0x2B, "swr": 0x2E, "lwc2": 0x32, "swc2": 0x3A}
REGIMM = {"bltz": 0, "bgez": 1, "bltzal": 0x10, "bgezal": 0x11}


def r(fn, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | FUNCT[fn]


def i(op, rs=0, rt=0, imm=0):
    return (OPC[op] << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def regimm(name, rs, imm):
    return (1 << 26) | (rs << 21) | (REGIMM[name] << 16) | (imm & 0xFFFF)


def j(op, addr):
    return (OPC[op] << 26) | ((addr >> 2) & 0x3FFFFFF)


def cop0(sub, rt, rd):
    return (0x10 << 26) | (sub << 21) | (rt << 16) | (rd << 11)


def mfc0(rt, rd): return cop0(0, rt, rd)
def mtc0(rt, rd): return cop0(4, rt, rd)
def rfe(): return (0x10 << 26) | (0x10 << 21) | 0x10


def cop2(sub, rt, rd):
    return (0x12 << 26) | (sub << 21) | (rt << 16) | (rd << 11)


def mfc2(rt, rd): return cop2(0, rt, rd)
def cfc2(rt, rd): return cop2(2, rt, rd)
def mtc2(rt, rd): return cop2(4, rt, rd)
def ctc2(rt, rd): return cop2(6, rt, rd)
def cop2cmd(imm25): return (0x12 << 26) | (1 << 25) | (imm25 & 0x1FFFFFF)
def bc2(true, imm): return (0x12 << 26) | (8 << 21) | ((1 if true else 0) << 16) | (imm & 0xFFFF)


def brk(code20): return ((code20 & 0xFFFFF) << 6) | 0x0D
def nop(): return 0
