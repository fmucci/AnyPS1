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
**Validation (observed):** frames compared with an independent emulator's neon GPU plugin at the same game state: after two fixes **99.70 %** identical pixels on a 3D gameplay frame and **99.56 %** on a 2D menu frame. The fixes: (1) *untextured* semi-transparent polygons take the blend mode from the draw mode (E1 / GPUSTAT bits 5-6), like rectangles and lines — a texture-page word does not exist for them; (2) interpolated vertex colours **and** texture coordinates must be *rounded* (`+ area/2` in the barycentric division), not truncated (rounding UV alone: 97.0 %, colour alone: 86.0 %, both: 99.7 %). Remaining 0.3 %: polygon edge pixels. **Limits:** barycentric rather than the hardware's incremental interpolators; no 1/16 sub-pixel rule; instantaneous drawing; lines use fixed-point major-axis stepping (see below); both implementations are not hardware, so agreement shows consistency, not exactness. A presenter must apply the vertical display range GP1(07) (a 14-line offset appeared when it was ignored).

## Observed usage patterns (useful to know what to test)
All of dither on/off, semi-transparency modes 0..3 and 4/8/15-bit textures occur in real 3D game frames; the mask bit stayed off in the programs
checked; texture window stayed at 0; per-viewport draw-area changes are frequent (hundreds of distinct E3/E4 values in a 3D scene); movie frames arrive
as many small CPU→VRAM uploads (strips), palettes as many 1×1 or 1×256 uploads.

## Unknown
GPU timing and FIFO depth, GPU IRQ, interlace field behaviour, exact texture-cache behaviour.

Origin: f2b#0007, 0008, 0022, 0025, 0027, 0029.

## Conformance battery (observed)
A synthetic program that replays a GP0/GP1 word list (one primitive family or mode per 64×60 cell over a Gouraud backdrop: flat/shaded triangles and quads, dither on/off, semi-transparency modes 0–3 on every family, textured 4/8/15-bit raw/modulated/semi triangles/quads/rectangles, lines, poly-lines, fill, VRAM copy, mask bit) was run in our model and in an independent emulator: **99.61 % identical pixels at first, 99.90 % after two fixes**. Both fixes were *missing features*, not rounding: (1) **poly-lines** (`0x48/0x58`: opcode word carries the first colour; each further vertex draws a segment from the previous one; the end marker `5xxx5xxxh` is only valid where a colour (shaded) or vertex (flat) is expected) and (2) **line stepping**: k = major-axis length, k+1 pixels, end with the smaller x first, 32-bit fixed-point positions starting at the pixel centre with a −1024 bias (also on y when decreasing), slope rounded *away from zero*, colours in 12-bit fixed point with a half-unit start. Remaining 0.1 %: ±1 LSB Gouraud rounding and a few off-by-one texels on shaded textured quads. Method note: a self-made battery run through *both* implementations finds missing primitive types that game frames never exercise.

## GPUSTAT bit 31 (odd/even field) — observed
In vertical-interlace mode (GP1(08) bit 5 on the register, bit 22 of the status word) bit 31 reports the field being drawn/displayed and **alternates once per field**. Programs busy-wait on it: a loop reads the status word and spins until bit 31 differs from the value it saved. A model that returns 0 forever hangs such a loop with the machine otherwise healthy (virtual time keeps passing, the screen stays as it was). Returning the frame parity in interlace mode and 0 otherwise was enough for the program that does this (an end-of-credits sequence in 640×480 interlaced mode) to continue and match an independent emulator's animation. Not modelled: the real line parity within a field. Method note: a program that "freezes" with no fault is usually spinning on a status bit; decode the loop's load address first.

## Hardware-exact rules learned from public test programs (observed; captures of unstated provenance, probably a console)
Running public test programs (see 16, B1) through a recompiled pipeline and comparing the final VRAM with the PNGs shipped with them gave **pixel-identical** results (5-bit precision) for gouraud triangles, UV-interpolated quads, rectangles of every kind, texture flip, texture overflow, the four semi-transparency modes, clipping and overlapping VRAM copies. The rules that mattered, replacing earlier approximations:
- **Polygon colour and UV are planes in fixed point.** Per-pixel step = exact slope × 4096 truncated toward zero (slope = attribute determinant / area determinant); the value at a pixel is the attribute of the **leftmost vertex** (ties: the first of the first two, the third replaces it only if strictly smaller) × 4096 + 2048 + step_x × (x − x₀) + step_y × (y − y₀), shifted right by 12 and masked to 8 bits. The origin vertex matters (different vertices give different low bits). This removed every ±1 difference that barycentric rounding left (an independent emulator's frames went from 99.7 % to 100.00 % identical on 3D game frames).
- **Textured rectangle flip**: Y flip counts down from v0; X flip counts down from **u0|1** (capture evidence only for u0 = 0, where the first pixel uses 1; **documented** for other u0 by a Mednafen cross-reference in another project's notes: X flip uses a negative U step and forces `u|1`).
- **VRAM→VRAM copy**: rows top to bottom, each row read completely before it is written (overlapping copies).
- **Lines are dithered when E1's dither bit is on even if flat**; flat polygons are not.
- **GPUSTAT bit 15 (texture disable)** is written from E1 bit 11 and from a polygon's texpage bit 11 only while `GP1(09h)` bit 0 allows it; a write while not allowed clears it; toggling the allow flag does not change the stored bit.
- **Mask bit (E6)**: CPU→VRAM and VRAM→VRAM writes set bit 15 (E6.0) and skip pixels that have it (E6.1); the rectangle fill ignores both.
- A GPU **CLUT cache** exists (a test about it differs in 0.2 % of VRAM here): not modelled, rules unknown.
Method note: capture-based tests are only as good as their provenance and their binary: some shipped logs/PNGs come from an older build than the prebuilt executable, and a test that prints uninitialised memory cannot be pixel-exact.
