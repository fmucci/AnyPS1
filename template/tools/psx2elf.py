import struct, sys
# usage: psx2elf.py <PS-X EXE> <out.elf>
# Wrap a PS-X EXE as an ELF32 LE MIPS so REA/Ghidra can open it.
# NOTE: R3000 is MIPS-I; EF_MIPS_ARCH_32R2 + abiflags are a workaround for REA's MIPS lane.
src, dst = sys.argv[1], sys.argv[2]
d = open(src,'rb').read()
assert d[:8] == b'PS-X EXE'
pc, gp, addr, size = struct.unpack('<IIII', d[0x10:0x20])
body = d[0x800:0x800+size]
shstr = b'\0.text\0.shstrtab\0.MIPS.abiflags\0'
abiflags = struct.pack('<HBBBBBBIIII', 0, 32, 2, 1, 1, 0, 1, 0, 0, 0, 0)  # v0, isa 32r2, gpr32, fp double, cpr1 32
off_body = 0x1000
off_abi = off_body + len(body)
off_abi = (off_abi + 7) & ~7
off_shstr = off_abi + len(abiflags)
off_sh = (off_shstr + len(shstr) + 3) & ~3
ehdr = b'\x7fELF' + bytes([1,1,1,0]) + b'\0'*8
ehdr += struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, pc, 52, off_sh, 0x70001000, 52, 32, 2, 40, 4, 2)
ph  = struct.pack('<IIIIIIII', 1, off_body, addr, addr, len(body), len(body), 7, 0x1000)
ph += struct.pack('<IIIIIIII', 0x70000003, off_abi, 0, 0, len(abiflags), len(abiflags), 4, 8)  # PT_MIPS_ABIFLAGS
n_text, n_shstr, n_abi = 1, 7, 17
sh  = struct.pack('<IIIIIIIIII', *([0]*10))
sh += struct.pack('<IIIIIIIIII', n_text, 1, 7, addr, off_body, len(body), 0,0,4,0)
sh += struct.pack('<IIIIIIIIII', n_shstr, 3, 0, 0, off_shstr, len(shstr), 0,0,1,0)
sh += struct.pack('<IIIIIIIIII', n_abi, 0x7000002a, 2, 0, off_abi, len(abiflags), 0,0,8,24)
out = (ehdr+ph).ljust(off_body, b'\0') + body
out = out.ljust(off_abi, b'\0') + abiflags + shstr
out = out.ljust(off_sh, b'\0') + sh
open(dst,'wb').write(out)
print(dst, hex(addr), hex(pc), len(body))
