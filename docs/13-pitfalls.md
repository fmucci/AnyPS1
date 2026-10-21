# 13 — Pitfalls (each cost real time)

| # | symptom | cause | fix |
|---|---------|-------|-----|
| 1 | Differential trace diverges only in `a1..a3` of an HLE line inside a callback | host→guest calls left argument registers as the interrupted code had them in one executor, zero in the other | set all four argument registers explicitly in *every* host→guest call, in both executors |
| 2 | `indirect call to X: no generated function` right after a table `jr` | jump-table recogniser accepted `addu` only; code used `add` | accept both |
| 3 | Function ends early at `syscall` | emitter treated it as always-fatal | handle syscall 1/2 inline and continue |
| 4 | Divergence at a single event in the middle of a long run | self-modifying instruction (patched argument load) | patch table + store guard (02) |
| 5 | `unknown GP0 command 0809xxxx` | ordering-table node one word too long; tag sent as data | GP0 opcodes 04h..1Eh are NOPs |
| 6 | "DMA channel N started while disabled" abort | real hardware keeps the channel busy and starts it when DPCR enables it | model pending DMA |
| 7 | A movie plays fully, another stalls at its last frames (`Retry` loop) | CD rate credited per internal step instead of per frame (1.6× too fast) | credit once per frame; check device rates per tick before debugging the consumer |
| 8 | MDEC picture slightly wrong everywhere | used the documentation's approximate IDCT/rounding; double instead of single precision | see 09; verify against an independent implementation on random input |
| 9 | Program reads the kernel area (`load at 0x200`) | null table pointer; real BIOS content hides it | allow narrow low-RAM range, document, test sensitivity |
| 10 | Pad never seen | kernel software vblank event (`F2000003/2`) missing | deliver it every frame |
| 11 | Card events delivered inside the call are lost | program enables callbacks after the call returns | deliver on the next frame |
| 12 | DICR byte writes ignored | only word writes handled | merge sub-word writes on DPCR/DICR |
| 13 | Python dies in `ctypes` call into the device library | missing `argtypes` truncates pointers to 32 bit | declare argtypes/restype for every function |
| 14 | ASan binary hangs at start (macOS toolchain) | ASan runtime deadlock in its own init | use UBSan |
| 15 | UBSan: left shift of negative value in fixed-point code | `(t << 12)` on a negative sample | multiply instead of shifting |
| 16 | `timeout` missing, long `sleep` blocked (macOS, agent shells) | platform | `perl -e 'alarm N; exec @ARGV'`; poll files with `until` loops |
| 17 | Overlay's own code stops the program with a fatal "store into recompiled code" after the overlay is freed | memory reused by the guest allocator | unregister the module on the first store (02) |
| 18 | Waiting on an event forever after returning from an overlay (SPU transfer never completes) | a Stop→Exec→Reset sequence wiped DMA/IRQ enables; the program's wait only ends because a BIOS `CdRemove` closed the *stale* CD-event handle that now names the SPU event | model the BIOS's 5 CD events in slots 0..4 and CdRemove closing their stored handles (04); found by running the same program in an independent emulator and diffing the event table |
| 19 | No reference for a behaviour question | no console/BIOS image available | run the program in an independent emulator with scripted input and a PC/BIOS-call trace (see 00); treat its HLE BIOS as evidence, not truth |

- **A sequence never advances, nothing faults** → a busy-wait on a status bit the model never changes (GPUSTAT bit 31 field flag, see 06). Find the loop from the last pc, decode the load address, implement the bit.

- **The game draws and runs but plays no music or effects** → no voice key-ons: look for an unmodelled timer interrupt (root counter 2 at 100 Hz drives a sequencer, see 05). Compare key-on counts or audio RMS per second against an independent emulator.

- **A program ignores input although the pad value reaches its buffer** → the code that reads the pad is a vblank event callback (`OpenEvent(F2000003, 2, 0x1000, func)`): if the routine that registers it was replaced by an early stub, or the callback has no recognisable start (no prologue, no `jr ra` before it) so discovery never emitted it, it never runs. Remove stale stubs once the event layer exists and give discovery explicit roots for such callbacks.

- **A public test program fails although the model looks right**: check (a) its header SP (0 → give it a stack), (b) reads of the BIOS area (version string at 0x180 → zeros), (c) BIOS `printf`/`puts` (A(3Fh)/A(3Eh)), (d) jump tables the pattern matcher does not know (treat runs of code pointers in data as entries), (e) whether the expected output is from a different build than the binary, (f) uninitialised variables printed or drawn by the test itself.

## Tooling notes
Disassemblers can mis-decode COP2 in delay slots and tiny leaf functions without prologues; check raw words. A raw-word scan for `mfc0/mtc0` finds
data. Keep generated C and every game-derived artefact out of version control.
