# 21 - Dry run on a title the toolchain was not built around (chain-loaded launcher title, third SDK build)

Date 2026-10-23. The first port's disc (a launcher executable that loads four others by name) was instantiated from a clean copy of `template/` **following only the PLAYBOOK**, as an agent with no memory of that port would, to find what a newcomer would trip on. **Observed** outcome: the frame at tick 12000 is byte-identical to the one the first port's own repository produces.

## Steps and what each needed
1. `init_game.py` on the .cue: boot file found from SYSTEM.CNF (a 28 KB launcher). Nothing else needed.
2. `find_hooks.py --write`: 2 SDK functions recognised in the launcher (the signature database already held this SDK build).
3. First run, 3000 ticks: `store at 80009ff0: first 64 KiB of RAM belongs to the BIOS` -> the launcher's own stack lives below 0x10000 (first store at the stack top, right at a function prologue). Policy of docs 04: allow exactly that range in `game.json` `low_ram` with the evidence. **Friction:** an agent must recognise "pc is a prologue, address just below the initial SP" as a stack, not a stray pointer; the triage table now says so.
4. 40000 ticks: no progress and repeated `CD timeout: CD_read:(CdlReadN)` -> the **time base**: this SDK build's CD driver counts frames with a timeout, and a virtual frame of 40000 backward branches (the other port's value) holds too much work. `tools/calibrate_time.py` reproduces it: 54 timeouts at 40000, 0 at 4000 (and at 1000). `yield_period` is now a per-title setting in `game.json` (default 40000), passed to the C runtime and the oracle.
5. `DoExecute: no recompiled module for INTRO.EXE` -> the launcher loads other executables by name: list them in `exes` (the first is the boot file, the others are linked as chain modules; `build_game.py` generates `rt_hle_add_module`). Fix: add the four names.
6. Result: the title's intro credits render. Overlays loaded from `.EXR` files (relocated by the title's own relocator function) are the next layer: `overlays` entries need the relocator's address, which static analysis of the host executable must supply (docs 02); not attempted here.

## What this changed in the template
`yield_period`; chain modules from `exes[1:]`; parametrised `exr.py` (relocator/scratch from `game.json`); `tools/calibrate_time.py`; two triage rows. Everything else worked unchanged: the two titles and the 48 public programs now run on one toolchain.

## Still needing a human-grade judgement (state it in the final report)
Which of two plausible `low_ram` ranges is the real one; the time-base value beyond "largest with no timeouts" (confirm against the reference emulator); overlay relocator discovery.
