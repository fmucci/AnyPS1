# 09 — MDEC (movie decoder)

## Command set
Command 1 decode macroblocks (depth 4/8/15/24 bit, signed/unsigned output, bit-15 set), command 2 load quantisation tables (64 luma then 64
chroma bytes), command 3 load the scale table (64 halfwords), control/status registers, DMA 0 in / DMA 1 out. Colour macroblock order: Cr, Cb, Y1..Y4;
output 16×16 pixels (24-bit: 768 bytes, 15-bit: 512 bytes per macroblock). Mono: one Y block per macroblock.

## Decode details that matter for exactness (observed: 300/300 random streams identical to a hardware-derived reference algorithm)
- RLE: skip `0xFE00` padding; DC = `signext10(n) * q[0]` (quant scale does not apply to DC); if the macroblock's quant scale is 0 the coefficients are
  `signext10(n)*2` and are stored unreordered; otherwise AC = `(signext10(n) * q[k] * scale + 4) / 8` (C division, truncating), clamp to −0x400..0x3FF,
  stored through the zig-zag inverse.
- **IDCT**: two passes in 64-bit integer arithmetic with the scale table **as uploaded** (the documentation's table has row = frequency; the reference
  stores its transpose), pass 1 `t[x+y*8] = Σu blk[u*8+x]·S[u*8+y]`, pass 2 `Σu t[u+y*8]·S[u*8+x]` rounded as `(sum>>32) + ((sum>>31)&1)`; the result
  is sign-extended from 9 bits and clamped to −128..127 **before** colour conversion. The documentation's pseudo-code (`(sum+0FFFh)/2000h` per pass)
  is explicitly approximate and gives different pixels (every random stream differed).
- **YUV→RGB** in single precision with truncation toward zero: `R = Y + (int)(1.402f·Cr)`, `G = Y + (int)(−0.3437f·Cb − 0.7143f·Cr)`,
  `B = Y + (int)(1.772f·Cb)`, clamp −128..127, +128 when unsigned. Double precision changes some pixels.
- Quick sanity test: a DC-only block must decode to a flat picture; if it doesn't, the scale table orientation is wrong.

## Unknown
Timing; exact hardware behaviour for out-of-range streams (garbage in → garbage out is not verified).

Origin: f2b#0025, 0028.

## A block needs no end-of-block word when it is full (observed)
A public MDEC test streams one 8×8 block of exactly 64 coefficients (1 DC + 63 AC) with **no** end-of-block word and the hardware decodes it: once the 64th coefficient is stored the block is complete. A decoder that insists on reading one more word waits forever (the program then spins on the MDEC status). Streams that do end with an end-of-block word are unaffected (the word is skipped at the start of the next block).

## DMA stalls, they do not complete at once (observed, second title)
A streaming movie player issues, per frame, one RAM->MDEC input DMA and one MDEC->RAM output DMA **per 16-pixel strip**, and prefetches the first strip of the next frame before that frame's input exists. A model that completes every transfer immediately (a) hands the prefetch DMA zeros, which the player then draws, and (b) accepts the next frame's input while the current output is unread, so the decoder buffer is replaced after the first strips: symptom = one strip (or black) on screen where the reference shows full frames, and hundreds of output DMAs per input DMA in the log. Model: an output DMA moves only what is decoded and stays busy (no interrupt) until more output exists; an input DMA is held while output is unread. Whether the model still decodes the whole frame first (not streaming) is a separate simplification.

### The busy bit gates the player's strip loop (observed, second title)
A streaming player polls the MDEC status **command-busy bit (29)** between strips (the SDK's `DecDCTinSync(1)`): busy = the frame is still being decoded or its output has not been read out; not busy = frame finished, so it swaps buffers and restarts the strip x at the buffer origin. A model that shows busy only while input words are being received makes the player end every frame early: strips missing (a black bar at the right), then every later strip drawn at x = 0, and callbacks that pace the playback fire out of step. Reference behaviour: busy from the command until the last block has been read out; the RAM->MDEC input transfer of a frame stays busy until then (it completes when its data are consumed). Also: complete DMA interrupts a few hundred instructions after the start, not at the next vblank, or each strip callback costs a frame. Check: log the CPU->VRAM uploads of the player (x per strip) and compare with a reference emulator (20 strips of 24 halfwords per 320-pixel frame).

