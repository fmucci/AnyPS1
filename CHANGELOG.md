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
