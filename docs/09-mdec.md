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
