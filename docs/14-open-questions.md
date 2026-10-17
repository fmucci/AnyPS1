# 14 — Open questions (platform side)

1. **Hardware validation.** CPU semantics were checked against psx-spx and one independent emulator, not against a console or hardware test
   programs. GPU rasterisation and SPU output were never compared with any reference (no recorded hardware output, no runnable reference renderer).
2. **Library re-registration after Exec** (see 12/13 #18): after an overlay is executed between a `Stop` and a `Reset` of the callback library, a
   later SPU transfer waits for an event that no callback will deliver. The translated code runs exactly the instructions the console would;
   either an earlier branch differs (state we model differently), or the original relies on something not yet identified. Needs a reference
   trace from hardware or from an independent emulator running the same program.
3. **Kernel area content** read through null pointers: the real BIOS values are unknown; sensitivity tests show no visible effect so far.
4. **Timing**: all devices are instantaneous or constant-delay; programs that depend on real timing (hard polling loops with magic counts) are untested.
5. **DMA chopping, GPU/CD/MDEC FIFO depths, interlace field timing** are unmodelled.
6. **SPU details**: sweep volumes, SPU IRQ, capture buffers, XA 25-point resampler, 8-bit XA.
7. **Second memory-card slot, multitap, analog pads, light guns**: not exercised.
8. **Region differences** (PAL/NTSC, BIOS versions): only one disc and no BIOS were used.
9. **Linux/other hosts**: the toolchain was only exercised on macOS (arm64, Apple clang).
