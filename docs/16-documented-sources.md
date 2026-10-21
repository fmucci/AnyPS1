# 16 — Documented sources (external knowledge we can use but have not re-verified)

Most of what AnyPS1 says is **observed** (reproduced here with a command). Much of what is still missing is already known to the community. This
page is the catalogue of that outside knowledge, so a session can go straight to the right place instead of re-discovering it. A fact taken
from here is tagged **documented** (see 00): *stated by an external source we consulted, not reproduced by our own test*. It becomes **observed**
only when one of our tests agrees with it, and **differs** when one disagrees.

Checked on 2026-10-18: repository existence, licence and file listings were read from the public GitHub API; contents marked "read" were
read, those marked "listed" were only found by search and not opened. Provenance statements the sources do not make are marked unknown.

## How to use a source (licence rules)
| licence | what is allowed here |
|---------|----------------------|
| MIT / permissive | read, quote short facts, reuse test programs and expected outputs with the licence notice kept |
| GPL / CC-BY-NC-ND / no licence stated | read for *behaviour* only; describe it in our own words; never copy or paraphrase line by line; instrumented copies stay local and out of git |
| documentation with unclear licence | cite and summarise facts; do not republish pages |

## A. Specifications
| id | source | covers | status |
|----|--------|--------|--------|
| A1 | **psx-spx** (nocash's PSX specs, community fork at psx-spx.consoledev.net; maintainers say no licence was formally acquired and rely on fair use) | memory map, I/O map, GPU, GTE, MDEC, SPU, interrupts, DMA, timers, CD-ROM drive and format, controllers and memory cards, SIO, memory control, unpredictable things, CPU, kernel (BIOS), hardware numbers | read (section list); primary spec already used throughout. Its own caveat: the development version contains recent AI-written changes, so prefer the stable version and cross-check anything surprising |
| A2 | BlueRetro project log "PlayStation/PlayStation 2 SPI interface" (Hackaday) | one SPI-style bus shared by pads and cards, chip select by first byte (pad 0x01, card 0x81), ACK line, LSB first, about 250 kHz (multitap 500 kHz–1 MHz), multitap enabled by the 3rd TX byte = 0x01 and adds one frame of delay | listed (search summary only). Secondary source from packet sniffing; its multitap *memory-card* part is marked TBD by its author |

## B. Hardware test programs (the missing hardware reference)
| id | source | licence | covers | status |
|----|--------|---------|--------|--------|
| B1 | **JaCzekanski/ps1-tests** | MIT (last push 2023-03) | CPU (access-time, code-in-io, cop, io-access-bitwidth), DMA (chain-looping, chopping, otc-test), GPU (bandwidth, benchmark, clipping, clut-cache, gp0-e1, gpustat, lines, mask-bit, quad, rectangles, texture-flip, texture-overflow, transparency, triangle, uv-interpolate, vram-to-vram-overlap, version-detect), GTE (gte-fuzz, test-all), MDEC (4bit, 8bit, frame, movie, step-by-step-log), SPU (memory-transfer, ram-sandbox, stereo, test, toolbox), timers, pad input, CD-ROM (disc-swap, getloc); `tools/diffvram` compares two VRAM images | **used** (f2b finding 0043): 9 GPU tests pixel-identical, 3 logs identical, seven rules corrected (06, 09). Many tests ship an expected-output artifact (`vram.png`, `psx.log`); **provenance not stated** (console or emulator unknown); some artifacts are from older builds than the prebuilt executables; gte-fuzz is described as usable to verify against a real console. Needs: nothing but the release zip, the programs recompile with a generic recompiler once unresolved code pointers and jump tables are added as entries |
| B2 | simias/psx-hardware-tests | none stated (no LICENSE file; last push 2020-02) | test programs and a small runtime/lib | listed only; read-only reference |
| B3 | PeterLemon/PSX | none stated | assembly demos and tests | listed only; read-only reference |
| B4 | Amidog's CPU/GPU test programs | not checked | named on the GameTechWiki "PS1 tests" page | listed only; location and licence not verified |

Use: build the programs (they need a MIPS cross compiler, not installed in our environment), run them in our model, and diff the produced VRAM/log against the
shipped expected output. If the expected output is hardware, this answers open question 1; if it is from an emulator it is one more independent reference.

## C. Open BIOS and SDK sources (readable ground for "what does the kernel do")
| id | source | licence | covers | status |
|----|--------|--------|--------|--------|
| C1 | **OpenBIOS** (in `pcsx-redux/nugget`, `openbios/`: kernel, cdrom, card, sio0, fileio, handlers, gpu, shell, boot, tty) | MIT | a from-scratch BIOS able to boot most games; `COMPATIBILITY.md` lists two NTSC titles known not to work | read: `kernel/events.c`, `cdrom/events.c`, `cdrom/cdrom.c` (event parts), `COMPATIBILITY.md`. It is a *re-implementation*, so it shows a valid behaviour, not necessarily the retail BIOS's |
| C2 | PSn00bSDK (Lameguy64, community fork spicyjpeg) | mixed (MPL for parts, some code stated to derive from Psy-Q disassemblies; GitHub reports no single licence) | free SDK whose library API imitates Sony's: GPU, GTE, CD-ROM, MDEC, interrupt dispatch | listed; useful as a readable analogue for 12, not as a code source |
| C3 | nugget's PSYQo C++ SDK | MIT | bare-metal libraries and examples | listed |

## D. Emulator sources (behaviour oracles; licence-limited)
| id | source | licence | use |
|----|--------|---------|-----|
| D1 | PCSX-ReARMed (libretro) | GPL-2.0 | run headless as the independent reference (00); instrumented copy local |
| D2 | PCSX-Redux | GPL-2.0 (OpenBIOS inside is MIT, C1) | accurate reference with debugger; candidate second oracle |
| D3 | DuckStation | no SPDX; forbids derivatives | read for behaviour only (see 15) |
| D4 | Beetle PSX (Mednafen-derived libretro core) | GPL-2.0 | candidate second oracle for GPU/SPU |

## What these sources settled or can settle
| open question (14) | documented lead | state |
|---|---|---|
| 1 hardware validation | B1 expected outputs, gte-fuzz; B4 | not yet run; provenance of the expected outputs unknown |
| 2 BIOS event-slot lifecycle | **C1 read**: `OpenEvent` takes the first free control block (flags = 0) and returns `0xF1000000 \| slot`; `CloseEvent` frees the block; the CD code opens exactly five events of the CD class in the order specs 10h, 20h, 40h, 80h, 8000h and closes those five stored handles when it is shut down; `WaitEvent` returns 1 if ready (and re-arms), blocks if enabled, returns 0 for any other state (closed/disabled); handles are decoded by their low 16 bits only | **documented**, agrees with our model (04). Still open: that the *retail* BIOS keeps the five handles in the same slots 0..4 |
| 3 kernel area content | C1 shows its own kernel globals; they are not the retail layout | no help; needs a BIOS dump we do not have |
| 4 timing | B1 `cpu/access-time`, `timers`, `gpu/bandwidth` logs | not yet used |
| 5 DMA chopping, FIFO depths | B1 `dma/chopping`; A1 DMA and GPU sections | not yet used |
| 6 SPU details | B1 `spu/*`; A1 SPU section | not yet used |
| 7 second slot, multitap, analog pads | A1 "Controllers and Memory Cards" and SIO; A2; B1 `input/pad` | not yet used |
| 8 regions | A1; C1 compatibility notes | no source with region-specific measurements found |
| 9 Linux | — | needs a Linux run, not documentation |

## Rule
Add a source only after opening it; record its licence from the repository, not from a search summary; say what it covers, what we read, and what it settled.
