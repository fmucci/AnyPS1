# 07 — GTE (coprocessor 2)

- Register file semantics (data/control moves, `SXYP` FIFO push, `IRGB→IR1..3`, `LZCS→LZCR`, FLAG bit 31 summary) plus every command of the
  psx-spx list; `lwc2/swc2` and the move instructions go through the same functions. Unknown command codes abort.
- **Verification**: differential test of all 64 registers after each command against an independent implementation built locally from its public source
  (stripped of emulator dependencies, never committed): 400 000 random commands, zero differences (random full-range, small, fixed-point-typical and edge
  values; random `sf/lm/mx/v/cv` fields for 22 command codes). The same C code is shared by runtime and oracle, so they cannot disagree.
- Delay-slot COP2 reads and the load delay of `mfc2/cfc2` must be handled by the translator (01); real code relies on both.
- Disassemblers (tested: Ghidra) may mis-decode COP2 instructions in delay slots; check raw words.

## Unknown
Instruction timing; behaviour with invalid register numbers or a command issued while another "is running" (not modelled).

Origin: f2b#0009, 0015, 0024.
