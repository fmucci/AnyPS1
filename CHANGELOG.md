# Changelog

- 2026-10-17 · Repository created with docs 00–15, extracted from the platform findings of the first port (CPU semantics and tests, recompiler design, memory/EXE/EXR
  formats, BIOS layer, IRQ/DMA/timing, GPU, GTE, CD-ROM, MDEC, SPU, memory card, SDK conventions, pitfalls, open questions, sources). No title-specific content.
- 2026-10-17 · 04 (BIOS events: 5 CD events in slots 0..4, CdRemove closes stored handles, WaitEvent semantics), 13 (#18 resolved, #19 added), 14 (event-slot lifecycle replaces the library re-registration question), 00 and 15 (independent emulator as behavioural oracle).
- 2026-10-17 · 06-gpu: rasteriser validated against an independent emulator (99.7 % identical pixels), the two fixes (untextured semi mode from E1, rounded attribute interpolation), presenter must apply GP1(07).
- 2026-10-17 · 10-spu: XA validation against an independent emulator (shape matches, gain 1.26× unexplained).
