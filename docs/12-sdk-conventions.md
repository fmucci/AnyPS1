# 12 — SDK (Psy-Q-style) conventions visible in binaries

Observed across five executables built with one SDK build (library identity strings and layout were identical):
- Libraries communicate through **RAM tables of function pointers** (graphics initialisers, interrupt/callback table, CD) set up by `Reset*`/`Init*`
  functions, so API-level replacement is possible once per table entry; wrappers are small functions `lw v0,table(...); lw v0,N(v0); jalr`.
- **Callback library**: a table of entries (e.g. reset, interrupt-callback, DMA-callback, stop, restart) plus a flag word. `Reset` is a no-op when the
  flag is set and a **full reset when it is clear** (it zeroes DPCR/DICR, installs handlers); `Stop` saves I_MASK/DPCR, disables DMA, clears the flag;
  `Restart` restores them when the flag is clear. Programs call the "ensure initialised" wrapper at the start of many library functions. A
  Stop → Exec → Reset sequence therefore wipes DMA/IRQ enables for *every* library; each library must re-register its callbacks afterwards.
- **SPU library**: init sets `DPCR |= 0x000B0000` and registers the DMA-4 callback (DMA callback = table entry 1, sets the DICR channel enable);
  transfers wait on an event; a "quit" clears the callback and closes the event.
- **setjmp** is used by the interrupt layer to build the exception-exit buffer (see 02, 04).
- Stream/movie code uses the CD library's ring (08) and the MDEC library's DMA helper; decoding is driven from vblank-synchronised callbacks.
- Programs print diagnostics through the console file handle (`FileWrite` fd 1): capture it, it is a free trace.
- Library strings in binaries include source file names and revision numbers: useful to identify the exact SDK build.

Origin: f2b#0005, 0006, 0014, 0027.
