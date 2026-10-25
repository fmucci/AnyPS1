# 10 — SPU

## Model (psx-spx formulas; unit-tested with hand-derived expectations; hashed output compared between executors)
- 24 voices, 44.1 kHz output, 735 stereo frames per 60 Hz tick into a ring (a frontend drains it) plus a hash of all samples.
- **ADPCM** blocks of 16 bytes: shift (≤12, larger values act as 9), filter 0..4 with the standard coefficient pairs, flags bit0 loop end (sets ENDX, jump to
  repeat address), bit1 repeat (clear = release + level 0), bit2 loop start. Decode `(nibble·4096 >> shift) + ((old·f0 + older·f1 + 32) >> 6)` clamped to 16 bits
  (use multiplication, not a left shift of a negative value).
- **Interpolation**: 4-point Gaussian with the 512-entry table (transcribed from the documentation text by a script; 512 entries);
  pitch counter 12-bit fraction, step = pitch clipped to 0x4000, pitch modulation uses the previous voice's output.
- **ADSR**: attack/decay/sustain/release with linear and exponential modes, `cycles = 1 << max(0, shift−11)`, `step = step << max(0, 11−shift)`,
  exponential-increase slows above 0x6000, exponential decrease scales by the level; fixed-volume registers hold volume/2 (so `0x3FFF` = full scale);
  sweep volumes are applied as their end value (approximation). Reads of the current ADSR level and current volumes are answered from the model.
- **Noise**, key on/off, ENDX, main volume, CD-input volume, SPUCNT soft-reset/mute.
- **Reverb** with the psx-spx formula, one side per 44.1 kHz cycle, work area `mBASE..7FFFEh`; the commonly used SDK presets (e.g. "Studio Large": `mBASE =
  0xF204` for a `0x6FE0`-byte area) are recognisable in register logs.
- **XA-ADPCM** (CD-controller side): per 128-byte group header copies, filters 0..3, 4-bit, stereo or mono, 37 800 / 18 900 Hz (doubled), resampled to
  44.1 kHz by linear interpolation (the hardware's 25-point zig-zag filter is not modelled).
- SPU transfers by DMA 4 or manual writes; completion is signalled by the program's own DMA callback (see 04, 05).

## Typical register behaviour in real programs (observed)
Start-up keys off/on all 24 voices; fades by stepping main and CD volumes; reverb programmed once with a preset and enabled per voice via the EON mask;
noise and pitch modulation unused.

## SPU IRQ and the repeat address (observed, second port, finding 0019)
**Observed.** A game streamed the sound of its full-motion videos with the SPU IRQ, and a model without it plays every video in silence while everything else (music, effects, picture) looks right. The pattern: the program DMAs a buffer into sound RAM (here 8 KB at 1100h), writes the **ADPCM repeat address register (1C0Eh+16n) of two voices *before* key-on**, sets the IRQ address (1DA4h, in 8-byte units) at the start of the block just past the voices' loop end, sets SPUCNT bit 6, keys the voices on, and waits: when the voices reach the end the IRQ handler (I_STAT bit 9) DMAs the next chunk and moves the IRQ address to the other half. It repeated every 4 video frames for the whole video.
**Model that works.** (1) A write to 1C0Eh+16n sets the voice's repeat address; key-on keeps a repeat address that was written or set by a loop-start block (it only defaults to the start address if neither happened); the reference core does the same. (2) IRQ: SPUCNT bit 6 enables, SPUSTAT bit 6 is the flag, writing SPUCNT bit 6 = 0 clears it, I_STAT bit 9 is raised once per flag. (3) The IRQ address is checked against the 16-byte block a voice fetches **and against the block its pointer has stepped to when a loop-end flag sends it back** (the voices of this game end their buffer on a flagged block and the IRQ address lies in the block after it; checking only fetched blocks never fires).
**How it was found.** Silence only for some input paths looked like a state bug; the log of SPU DMA starts per 100 frames showed the reference doing one every 4 frames and ours none; the callback addresses of the reference's interrupt dispatcher showed they were called for I_STAT bit 9; logging the IRQ source inside the reference core (channel and position) gave the voice and block; logging repeat-address writes gave the missing register. The reference's own comment says all voices keep advancing when stopped and can raise the IRQ; this model does not do that (unknown whether a game needs it).
**Check.** SPU DMA starts per 100 frames and envelope correlation of the movie audio against the reference (0.98 at a constant shift), plus a unit test (`test_spu_irq`: IRQ raised, flag, acknowledge, repeat address survives key-on).

## Not modelled
IRQ from DMA or manual writes to sound RAM and from stopped voices, capture buffers, sweep volumes (end value), 25-point XA resampler, 8-bit XA, exact transfer timing (instantaneous).

Origin: f2b#0011, 0025, 0029.

## Validation against an independent emulator (observed)
XA audio of a movie intro: loudness envelope correlation 0.948 (after a constant boot-time shift), waveform correlation 0.938 on an aligned 4000-sample window. The apparent 1.26× gain was the *reference's* default 75 % output volume; with it set to 1.0 the gain is 0.968 and the waveform correlation 0.964 (resampling differences account for the rest). Always check a reference emulator's own output-volume setting before blaming the mixer. Cross-correlate on loud segments (silent windows divide by zero) and re-align per window.

## Envelope details that decide whether a game's voice allocator works (observed)
- A game can decide that a voice is **free** from the *current ADSR volume* register (voice offset 0Ch) alone: one SDK-style voice manager polls it for all 24 voices every tick and treats a voice as free only after it read 0 for many consecutive polls. An envelope that never reaches exactly 0 therefore makes every voice look busy for ever, the allocator rotates through all 24 voices instead of re-using the first free one, and notes stop cutting each other off (the mix gets several times louder).
- Two modelling mistakes that cause exactly that, both found by comparing against a public reference emulator:
  1. **Key-off must reset the envelope rate counter.** A sustain with a very slow rate (shift 31 = about 1M samples per step) leaves the counter running; if key-off only changes the phase, the release waits until the old counter runs out (up to about 23 s). Reference cores reset the step counter at key-off.
  2. **Exponential decrease must round down.** The step is `step * level >> 15` with an arithmetic shift; truncating towards zero (`/ 0x8000` in C) turns a small negative step into 0 and the level sticks at a few units above 0 for ever. Rounding down gives at least -1 per step, so the level reaches 0.
- How to detect it in any game: log, per voice, how many of the game's polls read a non-zero level (a good model leaves the high voices almost always at 0 and the low voices busiest) and compare with a reference core's distribution.

## Byte and word reads of SPU registers (observed)
SDK sound code reads a voice register with a byte load. The 16-bit-wide bus returns the addressed byte of the halfword register; a 32-bit read returns two consecutive registers. Do not abort on these; byte *writes* have not been observed.

## A keyed-on voice must not read level 0 when polled (observed, second title)
A music driver that allocates voices by reading the **current-volume register of every voice** (0 = free) polls right after it has keyed a voice on. If the model steps the SPU in bulk (per frame) the new voice still reads 0, the allocator reuses or mis-orders voices, more voices stay sustained and the mix is louder (here up to 2x) and decorrelated from a reference. Reference behaviour: the SPU is advanced to the current time before the register is answered, so a voice keyed a few thousand cycles earlier is already above 0. Minimal model: on a level read of a voice that is on, in attack and at level 0, run one envelope step first. Check: compare the number of distinct voices used and the per-frame RMS of the mix with a reference emulator (the ENDX register is not involved).

