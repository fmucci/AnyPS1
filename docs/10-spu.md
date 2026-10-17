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

## Not modelled
SPU IRQ address (register kept as storage), capture buffers, sweep volumes (end value), 25-point XA resampler, 8-bit XA, exact transfer timing (instantaneous).

Origin: f2b#0011, 0025, 0029.

## Validation against an independent emulator (observed)
XA audio of a movie intro: loudness envelope correlation 0.948 (after a constant boot-time shift), waveform correlation 0.938 on an aligned 4000-sample window. The apparent 1.26× gain was the *reference's* default 75 % output volume; with it set to 1.0 the gain is 0.968 and the waveform correlation 0.964 (resampling differences account for the rest). Always check a reference emulator's own output-volume setting before blaming the mixer. Cross-correlate on loud segments (silent windows divide by zero) and re-align per window.
