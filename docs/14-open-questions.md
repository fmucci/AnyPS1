# 14 — Open questions (platform side)

1. **Hardware validation.** *Partly answered (see 06 and 16 B1): GPU output is pixel-identical to the PNGs of public test programs whose capture provenance is not stated; still no first-hand console run.* CPU semantics were checked against psx-spx and one independent emulator, not against a console or hardware test
   programs. GPU rasterisation and SPU output were never compared with any reference (no recorded hardware output, no runnable reference renderer).
2. **BIOS event-slot lifecycle**: the CD events in slots 0..4 and the `CdRemove` closing stale handles are *inferred* (documented 5 BIOS CD events; an independent HLE kernel shows the same effect). The real BIOS's handle variable and slot assignment were not verified; if a program's event lands outside slots 0..4 the effect changes.
3. **Kernel area content** read through null pointers: the real BIOS values are unknown; sensitivity tests show no visible effect so far.
4. **Timing**: all devices are instantaneous or constant-delay; programs that depend on real timing (hard polling loops with magic counts) are untested.
5. **DMA chopping, GPU/CD/MDEC FIFO depths, interlace field timing** are unmodelled.
6. **SPU details**: sweep volumes, SPU IRQ, capture buffers, XA 25-point resampler, 8-bit XA.
7. **Second memory-card slot, multitap, analog pads, light guns**: not exercised.
8. **Region differences** (PAL/NTSC, BIOS versions): only one disc and no BIOS were used.
9. **Other hosts**: the toolchain ran on macOS (arm64, Apple clang), on Linux (aarch64, GCC, in a container) and as a Windows x86-64 cross build (MinGW-w64) run under Wine; not on native Windows or x86-64 Linux, and not with MSVC. See 02 for what portability needed.

Documented leads for these questions (hardware test programs with shipped expected outputs, an open-source BIOS, SIO/multitap notes) are listed per question in 16.

## Interpreter tier (answer found in the second title, see 02)
The long-standing question "what do we do with code that exists only in RAM after a load (overlays, callbacks nobody can name)?" has a practical answer: a small MIPS-I interpreter in the runtime, entered from the indirect-call dispatcher when no generated function is registered at the target. It runs on the same CPU state and memory API, calls generated functions/HLE/BIOS entry points through the normal dispatcher, models load and branch delay slots exactly (a write to the loaded register in the delay slot cancels the load; lwl/lwr merge with the pending value), and counts the virtual-time events like generated code. Cost: interpreted code is slower than translated code; value: no offline pipeline per overlay is needed to run the game, and the interpreted entry points tell which regions are worth translating later. Zero-filled ranges must not be turned into "functions" (see 02), otherwise direct calls into them bind to stubs and never reach the interpreter.
