# 06 — GPU

## Command decoding (observed, shared C model)
- GP0 packet lengths by opcode: polygons `0x20..0x3F` (3 or 4 vertices; +1 word per vertex when textured; gouraud adds colour words),
  lines `0x40..0x5F` (poly-line ends with a `0x5xxx5xxx` word), rectangles `0x60..0x7F` (variable size adds a word; textured adds a UV word),
  VRAM→VRAM `0x80..0x9F` (4 words), CPU→VRAM `0xA0..0xBF` and VRAM→CPU `0xC0..0xDF` (3 words + data), fill `0x02` (3 words), environment `E1..E6`
  (1 word), `1F` IRQ1.
- **NOPs (one word)**: `00h`, `03h`, `04h..1Eh`, `E0h`, `E7h..EFh` (psx-spx "mirrors of GP0(00h)"). Real display lists contain tag words that reach GP0
  as data (see 05), so unknown low opcodes must be NOPs, not errors.
- GP1: reset, reset FIFO, ack IRQ, display on/off, DMA direction, display start, horizontal/vertical range, display mode, info requests
  (texture window, draw area, draw offset readable). GPUSTAT reset value `0x14802000` was confirmed against psx-spx.
- Display set-up seen in all executables of one title: GP1(08) modes for 320×240 15-bit NTSC (interlaced and not), GP1(06)=`C60260`,
  GP1(07)=`04001E`; double buffering by switching the display start between y=0 and y=256.

## Rendering model (software, written from psx-spx)
Polygons flat/gouraud/textured (4-bit and 8-bit CLUT, 15-bit), raw/modulated, semi-transparency modes 0..3, ordered dither matrix on shaded
pixels, mask-bit set/test, draw area clip, draw offset, texture window, rectangles with X/Y flip (E1 bits 12/13), DDA lines, fill/copy/upload/download.
**Limits (unverified against hardware):** barycentric interpolation with truncation, not the hardware's incremental interpolators (colour
off by one LSB possible); no 1/16 sub-pixel rule; instantaneous drawing; lines are plain DDA. No reference comparison was possible (no recorded
hardware frames, no runnable reference renderer): this is the largest unvalidated area.

## Observed usage patterns (useful to know what to test)
All of dither on/off, semi-transparency modes 0..3 and 4/8/15-bit textures occur in real 3D game frames; the mask bit stayed off in the programs
checked; texture window stayed at 0; per-viewport draw-area changes are frequent (hundreds of distinct E3/E4 values in a 3D scene); movie frames arrive
as many small CPU→VRAM uploads (strips), palettes as many 1×1 or 1×256 uploads.

## Unknown
GPU timing and FIFO depth, GPU IRQ, interlace field behaviour, exact texture-cache behaviour.

Origin: f2b#0007, 0008, 0022, 0025, 0027, 0029.
