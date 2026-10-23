# 05 — Interrupts, DMA control, timing

## Virtual time
- A "tick" = a fixed number (4000) of taken backward branches = one 60 Hz frame of host-owned time. At each tick: timer 1 advances by the
  scanline count (263 NTSC / 313 PAL from the display mode), timer 2 by sysclk/8 per frame, `I_STAT` vblank is raised, the kernel vblank events are
  delivered, the CD controller steps, the SPU produces one frame of samples, then pending interrupts are delivered if `SR.IE` and `(I_STAT & I_MASK)`.
- Observed: ordering is what matters; the scale is arbitrary. But **device data rates must be correct per tick**: a CD model that credited
  its rate on every internal step (up to four per tick) ran 1.6× too fast and made a streaming ring fill faster than the program consumed it,
  stalling a movie. Credit rate once per tick.

## Interrupt delivery
Cooperative: at a tick boundary the runtime runs the program's own handler through the custom-exit buffer (04) and restores all registers after it
returns. Handlers that touch the registers of the interrupted code must not be assumed atomic with respect to *forward* code between yields, but
read-modify-write sequences without a backward branch inside are atomic in this model (real hardware can interrupt anywhere).

## DMA
- Channels used and modelled: 0 MDEC in, 1 MDEC out, 2 GPU (block and linked list), 3 CD-ROM, 4 SPU, 6 OTC (ordering-table clear). Sub-word writes
  to DPCR/DICR must merge with the old value (a program wrote DICR by bytes).
- **Master enable**: a transfer started while the channel's DPCR bit (3 + 4·n) is 0 does not run; the channel stays *busy* and **starts when
  DPCR later enables it** (confirmed in an independent emulator's `CanTransferChannel`). Aborting on this case was wrong.
- Linked lists: a node is `len<<24 | next` followed by `len` words; end marker has bit 23 of `next` set. **A node's length can be one word too
  long** (the next tag is then sent to the GPU as data); hardware treats the tag as a GP0 NOP (see 06).
- DICR: bits 24-30 flags acknowledged by writing 1; bit 31 master flag = bit 15 | (bit 23 & any flag & enable); IRQ3 on I_STAT when it rises.

## Timers
Registers only (value/mode/target); timer modes, targets and IRQs are not modelled (no program needed them).

## Unknown
DMA timing and chopping, GPU timing, nested interrupts.

Origin: f2b#0022, 0025, 0027.

## Root counter 2 drives a game's sound sequencer (observed)
A program may program root counter 2 **directly through its registers** (no BIOS `SetRCnt` call) as the tick of its sound sequencer: mode `0x258` = sysclk/8 (bits 8-9 = 2), reset on target (bit 3), IRQ on target (bit 4), repeat (bit 6), target `0xA560` = 42336, i.e. 33 868 800 / (8 × 42336) = 100 interrupts per second, with I_MASK bit 6 (IRQ6) enabled. A model that advances the counter but never raises IRQ6 gives a game that runs and draws normally but plays **no music or effects** (no voice is ever keyed on). Model: clock = sysclk (mode bits 8-9 = 0/1) or sysclk/8 (2/3); sync modes 0 and 3 stop the counter; with bit 3 the counter counts 0..target and resets (period target + 1), otherwise it wraps at 0xFFFF; IRQ on target (bit 4) and/or overflow (bit 5); repeat (bit 6) or one shot. Because the virtual frame is 1/60 s and the tick is 100 Hz, **several interrupts per frame must be queued and delivered one by one** (1.67 per frame here). Verify by counting voice key-ons per second. Not modelled: counters 0 and 1 interrupts (dot clock, hblank sources), counter 2 synchronisation modes 1 and 2.

## Host frame pacing with virtual time (observed)
With a virtual frame clock the host decides when a finished frame is shown. **Wait for the frame's wall-clock moment first, then present, then sample input**: presenting right after the computation and sleeping afterwards makes the interval between pictures follow the emulation cost of each frame (4–31 ms measured against a 16.7 ms target while the mean stayed exact). A cheap way to tell whether "movement feels odd" is a port problem or the game's own pace: count new pictures per second and the pixel change per picture for the same scripted input in the port and in an independent emulator; in one case they were identical (≈ 11 pictures per second in a heavy 3D scene), so the choppiness belonged to the game.

## Documented DMA2 timing data (from the log shipped with a public test; provenance not stated)
CPU→VRAM DMA of 8192 bytes, CPU cycles (33.8688 MHz), sync mode 1 (block) by block size: 1 → 22819, 2 → 12508, 4 → 7309, 8 → 4790, 16 → 3607, 32 → 2998, 64 → 2662, 128 → 2471 (GPU still busy at the end for the larger sizes); sync mode 0 without chopping → 2196; with chopping windows of 1 word per 1/2/4 CPU clocks → 16693, 20595, 22834. A model with instantaneous DMA reports 0 for all of them. Use as targets if DMA timing is ever modelled; the shape (cost per block shrinks as blocks grow, floor near 2.2k cycles for 8 KiB ≈ 0.27 cycles/byte) is the useful part.

## DMA channel register rules (observed on public self-checking test programs)
- **OTC (channel 6) CHCR**: only bits 24 (enable/busy), 28 (trigger) and 30 are writable; bit 1 (backwards address step) is hard-wired to 1; all other bits read 0. A write with bit 24 clear stores bits 28/30 and runs nothing; the transfer clears bits 24 and 28 when it completes (a chopping request is ignored).
- **SPU (channel 4) in sync mode 0 (start immediately)** begins on the enable bit (24) alone — the manual trigger (28) is not needed — and BCR holds the word count; for GPU, CD and MDEC channels the trigger is needed in that mode. (Plausible reason: the SPU request line is always active.)
- Sync mode 1 transfers to the SPU take time on real hardware (a program measured them and expects code to be able to run between blocks): instantaneous DMA models fail those checks.


## DMA completion interrupts must not be visible inside the handler that started the transfer (observed)
A model that moves the data at once and sets the DICR channel flag / I_STAT bit 3 immediately breaks SDK interrupt layers: the DMA dispatcher (read DICR, acknowledge the channel flag, call the channel's callback, loop while flags remain) calls a callback that starts the next transfer (MDEC or CD streaming); the new transfer completes instantly, the flag is set again, and the dispatcher loops for ever inside one interrupt (the host-side symptom: millions of instructions in a single handler, "step limit" in the nested handler run). Real transfers take time, so the flag appears after the handler returned.
Fix that worked: keep a per-channel "completion pending" mask, raise the flag/IRQ from the host's time-step code between handler runs (one channel per call, handler delivered after each), a few dozen times per frame. DICR semantics (psx-spx, confirmed): bits 24-30 are write-1-to-clear acknowledges, bit 31 is derived.

## Root-counter interrupts and the BIOS interrupt handler (observed, second title)
Interrupts can reach the program through the BIOS exception handler in two different ways in the same program: (a) the SDK's custom exit (`SetCustomExit`, a `setjmp` buffer whose pc is the library's handler; the handler dispatches I_STAT bits to registered callbacks, acknowledging each before calling it) and (b) the BIOS's own chains for the root counters. A program can enable a root-counter IRQ directly in I_MASK (SDK `StartRCnt`) without registering a callback in (a); then the library handler never acknowledges that bit, sees it pending on every invocation and, after a few thousand invocations, prints a timeout line with I_STAT:I_MASK. The part that is missing is the BIOS handler's job: for an enabled source it **delivers the event `F200000t` (spec 2)** and **acknowledges the I_STAT bit when the clear flag set by `ChangeClearRCnt(t, flag)` is 1** (defaults in one published BIOS reimplementation: timers 0-2 = 1, vblank = 0, the SDK sets it). Do this before running the custom exit.

## The virtual-frame length is a per-title constant (observed, two titles)
One virtual frame = a fixed number of taken backward branches (RT_YIELD_PERIOD). The two ports needed 4000 and 40000. Too long: the SDK's own frame-counted timeouts fire (a CD driver prints `timeout` and retries the read for ever); too short: the title runs slowly in virtual time. `tools/calibrate_time.py` runs candidates and counts driver timeouts and display flips; take the largest candidate without timeouts, then compare the frame of a known screen with the reference emulator. Reads of a root counter also advance it inside the frame (docs 05 above), which both titles needed.
