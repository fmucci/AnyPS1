# 01 — R3000A semantics for a translator

Source of truth: psx-spx + the MIPS-I definition; behaviours below were additionally checked in an independent emulator's CPU core.

## Observed (implemented in spec + emitter, 142 182 random fragments compared, 0 mismatches; 13/13 deliberate bugs caught)
- **Load delay**: a load, `mfc0`, `mfc2`, `cfc2` writes its register after the *next executed* instruction; that instruction still reads the old
  value. A branch right after a load reads old values and the load commits before the delay slot. `lwl/lwr` merge with a pending load of the same
  register (no delay needed). `mflo/mfhi/mthi/mtlo` have no visible delay in the cases checked.
- **Load into a register written by the next instruction**: the loaded value is lost (confirmed in the independent emulator: a write cancels the
  pending load; a second load into the same register wins). A chain of several loads into one register must be followed (two real emitter bugs
  were found only by random tests with long chains).
- **Branches**: link registers are written when the branch executes (before the slot); `bltzal/bgezal` write `ra` even when not taken; `jalr` reads
  its target before writing the link register. REGIMM decoding: bit 16 selects ge/lt; link only if `(rt & 0x1E) == 0x10` (the independent emulator
  does it this way; the simple rule "bit 20" differs but real code only uses canonical values).
- **Exception-capable instructions occur in delay slots** (`add/addi/sub`, `syscall`, `break`), and branch targets can land on another
  branch's delay-slot instruction; a translator must handle both.
- **Divide**: by zero and `0x80000000 / -1` give the psx-spx table values, no exception. `break` code is 20 bits (`break 7` = code `0x1C00` =
  BIOS "division by zero" check). `add/addi/sub` overflow raises exception 12.
- **Unaligned** `lwl/lwr/swl/swr` are implemented byte-wise in the spec; misaligned `lw/lh/sw/sh` raise address errors (fatal in a port).
- **COP0** subset used by programs: SR (12), Cause (13, read), EPC (14); `rfe` shifts the mode stack. Other registers: refuse until a trace
  shows use (a linear scan of binaries finds many `mfc0`-looking words, but they are data).
- **`syscall`**: `a0 = 1` EnterCriticalSection (returns previous IE in `v0`, clears IE), `a0 = 2` ExitCriticalSection (sets IE), then execution
  *continues after the instruction*. A translator that ends the function at `syscall` breaks code that uses it inline (found in a relocated overlay).
- **`bc2f/bc2t`**: COP2 condition flag reads false (psx-spx); no emulator semantics exist; zero uses were found in five executables.

## Inferred
- Exception PC in a delay slot: hardware reports the branch in EPC with BD set; our translator reports the slot instruction. Harmless while every
  exception is fatal.
- Zero-filled initial RAM is a deterministic choice, not a hardware fact; uninitialised reads are not detected.

## Unknown
- Behaviour against real hardware (only psx-spx and another emulator were available); cache isolation (`SR.IsC`) and COP0 side effects on
  interrupts are not modelled.

Origin: f2b#0017, f2b#0020.
