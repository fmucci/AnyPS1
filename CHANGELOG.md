# Changelog

- 2026-10-17 · Repository created with docs 00–15, extracted from the platform findings of the first port (CPU semantics and tests, recompiler design, memory/EXE/EXR
  formats, BIOS layer, IRQ/DMA/timing, GPU, GTE, CD-ROM, MDEC, SPU, memory card, SDK conventions, pitfalls, open questions, sources). No title-specific content.
- 2026-10-17 · 04 (BIOS events: 5 CD events in slots 0..4, CdRemove closes stored handles, WaitEvent semantics), 13 (#18 resolved, #19 added), 14 (event-slot lifecycle replaces the library re-registration question), 00 and 15 (independent emulator as behavioural oracle).
- 2026-10-17 · 06-gpu: rasteriser validated against an independent emulator (99.7 % identical pixels), the two fixes (untextured semi mode from E1, rounded attribute interpolation), presenter must apply GP1(07).
- 2026-10-17 · 10-spu: XA validation against an independent emulator (shape matches, gain 1.26× unexplained).
- 2026-10-17 · 10-spu: XA level agrees within 3 % once the reference's default 75 % output volume is accounted for.
- 2026-10-17 · 06-gpu: conformance battery method; poly-lines and line stepping (99.61 % → 99.90 % identical pixels vs an independent emulator).
- 2026-10-18 · new 16-documented-sources: catalogue of external specs, hardware test programs (ps1-tests etc.), open BIOS/SDK and emulator sources with licence rules and the open question each can settle; new evidence tag **documented** (00); 04: open-source BIOS agrees with the CD-event/CdRemove model.
- 2026-10-21 · 06-gpu: GPUSTAT bit 31 field flag alternates per field in interlace mode (programs busy-wait on it); 13: silent freezes are often status-bit waits.
- 2026-10-21 · 02: cross-host portability (identical frames on macOS/Linux/MinGW+Wine, what differed); 14: question 9 updated.
- 2026-10-21 · 05: root counter 2 as a sound-sequencer tick (100 Hz, several interrupts per frame); 13: silent game with normal video = missing timer interrupt.
- 2026-10-22 · 04: pad polled from an F2000003 vblank event callback; 13: input ignored when the callback's registration was stubbed or the callback was never discovered.
- 2026-10-22 · 02: callbacks discovery cannot see (explicit roots + unresolved-pointer lister); re-audit early stubs.
- 2026-10-22 · 06: hardware-exact rules from public test programs (plane-gradient interpolation with leftmost-vertex origin, flip, VRAM copy rows, line dither, texture-disable bit, mask bit); 09: full block without end-of-block; 14/16/13/00: provenance caveat, pitfalls, method.
- 2026-10-22 · 05: host frame pacing (wait, present, sample input) and how to tell port jitter from the game's own frame rate.
- 2026-10-22 · 16: section E (other PS1 recompilation projects, licences, what they offer per open question); 06: X-flip `u|1` now documented by a second source.
- 2026-10-22 · 06: CLUT cache rules (observed on a public test, pixel-identical).
- 2026-10-22 · 05: DMA channel register rules (OTC control bits, SPU sync-0 start).
- 2026-10-22 · 10: envelope facts that decide a voice allocator (current-volume polling, key-off resets the rate counter, exponential decrease rounds down).
- 2026-10-22 · 04/05/08/10: second-title findings (BIOS C library calls, setjmp/kernel-patch routines, deferred DMA completion IRQ, BFRD rewind rule, byte SPU reads, small-integer pointer arguments).
- 2026-10-22 · 02: discovery on a large monolithic executable (zero-fill runs, delay-slot entries, callbacks built across a jal, covered-body candidates, stub declarations).
