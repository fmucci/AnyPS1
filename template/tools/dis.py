"""Disassembler listing of a range of an executable of the disc: python3 -I tools/dis.py EXE_NAME FIRST_HEX END_HEX   (word addresses; MIPS-I, register names)."""
import sys,struct
import os
ROOT=os.path.join(os.path.dirname(os.path.abspath(__file__)),"..")
sys.path.insert(0,ROOT)
from tools.recomp import funcs, isa
R="$r0 at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".replace("$r0","zero").split()
def fmt(i):
    n=i.name
    if n in("lui",): return "%s %s,0x%x"%(n,R[i.rt],i.imm)
    if n in("lw","sw","lh","lhu","sh","lb","lbu","sb","lwl","lwr","swl","swr"): return "%s %s,%d(%s)"%(n,R[i.rt],i.simm,R[i.rs])
    if n in("addiu","addi","andi","ori","xori","slti","sltiu"): return "%s %s,%s,%d"%(n,R[i.rt],R[i.rs],i.simm if n in("addiu","addi","slti","sltiu") else i.imm)
    if n in("beq","bne"): return "%s %s,%s,0x%08x"%(n,R[i.rs],R[i.rt],i.branch_target())
    if n in("blez","bgtz","bltz","bgez"): return "%s %s,0x%08x"%(n,R[i.rs],i.branch_target())
    if n in("j","jal"): return "%s 0x%08x"%(n,i.branch_target())
    if n=="jr": return "jr %s"%R[i.rs]
    if n=="jalr": return "jalr %s,%s"%(R[i.rd],R[i.rs])
    if n in("sll","srl","sra"): return "%s %s,%s,%d"%(n,R[i.rd],R[i.rt],i.sa)
    if n in("sllv","srlv","srav"): return "%s %s,%s,%s"%(n,R[i.rd],R[i.rt],R[i.rs])
    if n in("mult","multu","div","divu"): return "%s %s,%s"%(n,R[i.rs],R[i.rt])
    if n in("mflo","mfhi"): return "%s %s"%(n,R[i.rd])
    return "%s %s,%s,%s"%(n,R[i.rd],R[i.rs],R[i.rt])
exe,lo,hi=sys.argv[1],int(sys.argv[2],16),int(sys.argv[3],16)
if exe.startswith("ram:"):                       # a 2 MiB RAM dump (PS1_RAMDUMP=file with build/ps1_game): code that exists only after a load
    class _R:
        def __init__(self, path): self.d = open(path, "rb").read()
        def w(self, a): return int.from_bytes(self.d[a & 0x1FFFFF:(a & 0x1FFFFF) + 4], "little")
    img=_R(exe[4:])
else:
    from tools.recomp import gamecfg
    img=funcs.Image(os.path.join(gamecfg.DISC_DIR,exe))
for a in range(lo,hi,4):
    w=img.w(a)
    print("%08x  %08x  %s"%(a,w,fmt(isa.decode(w,a))))
