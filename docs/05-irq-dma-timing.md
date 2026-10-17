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
