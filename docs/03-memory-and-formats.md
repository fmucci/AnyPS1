# 03 — Memory map policy and file formats

## Memory map used as a policy (psx-spx "Memory Map")
- KUSEG low 512 MiB / KSEG0 / KSEG1 are one physical space (`addr & 0x1FFFFFFF`); RAM 2 MiB at 0; scratchpad 1 KiB at `0x1F800000`
  (KUSEG/KSEG0 only); I/O at `0x1F801000..0x1F803FFF`.
- **Refuse** (abort with the address) until a trace proves use: RAM mirrors above 2 MiB, expansion regions, BIOS ROM, KSEG2 cache control.
- The first 64 KiB of RAM belong to the BIOS/kernel. Programs can legitimately read it: observed once, a program dereferenced a null table pointer
  and read the kernel vector area (a latent bug that real BIOS content hides). Modelling the area as zeros let the program run; sensitivity was
  tested by filling it with four different patterns (no visible change in the cases tried). A port should treat "read of BIOS area" as a flagged
  allowance, not silently.
- Programs may also use a *stack* in the low area (initial `sp` from the executable header can point below 64 KiB): allow narrow, documented ranges.

## PS-X EXE
Header (2 KiB): magic `"PS-X EXE"`, then `pc (0x10), gp (0x14), load address (0x18), size (0x1C)`, memfill start/size (0x28/0x2C), initial sp
base and offset (0x30/0x34), text at `0x800`. A launcher-style program loads the next executable with the BIOS `Exec`/DoExecute: save a few
registers in the header's reserved area, zero-fill the memfill region, set sp/fp to base+offset when base is non-zero, set gp, call the entry with
two arguments, restore and return 1 when the executable returns.

## PS-X EXR (relocatable overlay)
Layout observed: 2 KiB header (an exec-struct from +0x10: entry, gp, base written by the loader, memfill start...), a relocation area whose size is
`(count & 0x3FFFFE00)*4 + 0x800` for a count stored in the header, then the image. A relocator walks the list and adjusts, by opcode: `j/jal`
targets, `lui/addiu|ori|load|store` pairs with carry, plain words. Validation done offline: opcode histogram of relocation targets (all inside the
handled set), size equals the layout, every entry in range. The run-time size is image size + BSS size from the header.

## Launcher chains
A common pattern: a boot executable loops "load name from a 12-byte block, exec it"; each program writes the *next name* into the block and
returns. A port can replace the launcher by a host loop keyed on the name block while keeping the block as the hand-off channel (programs also
use it to pass flags/arguments).

Origin: f2b#0002, 0003, 0013, 0018, 0021, 0026.
