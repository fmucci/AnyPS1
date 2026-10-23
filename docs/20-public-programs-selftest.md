# 20 - Standalone programs through the whole agent flow (third-program test)

Date 2026-10-23. Purpose: the toolchain was written against two commercial titles; this checks that it works unchanged on **programs it was never tuned for** (the public `ps1-tests` release, PSn00bSDK-built, no disc, no SDK functions recognised). `tools/mkdisc.py` wraps each EXE in a raw MODE2/2352 disc, then the PLAYBOOK flow runs: `init_game` -> `find_hooks` -> `build_game` -> run 3000 ticks (`tools/selftest_public.py DIR`). **Observed** result: 43 of 48 programs run to the tick limit; 5 stop with a fatal that names a feature outside the model.

First run: 37 of 49 ran; the rest exposed gaps that were fixed in `template/` the same day (each is now a documented behaviour, docs 04/10/17):
| gap found | fix |
|---|---|
| header SP = 0 -> store at 0xFFFFFFF4 | boot sets SP = 801FFF00h when the header gives none |
| BIOS B(12h) InitPAD / B(13h) StartPAD missing | pad buffers filled every vblank: byte 0 status, 1 id 41h, 2-3 buttons (active low); port 2 absent (FFh) |
| serial port 1 (1F801050..5F) rejected | modelled as a sink: TX always ready, bytes go to the console capture, other registers plain storage at any width |
| `A(00h..05h)` file calls missing | aliases of B(32h..37h) |
| `B(47h/48h)` AddDrv/DelDrv, `A(3Bh)` getchar | success / 7Fh (no input) |
| COP0 register 15 (PRId) | returns 2 |
| 32-bit SPU register access | two halfword accesses, low first |
| functions that run into data (or contain deliberate illegal opcodes) were refused wholesale | such words become `break 0xFFFFF` traps: the function exists and the run stops with a fatal only if the word is *executed* |

## Remaining stops (expected; each is a documented gap, not a silent fallback)
| program | stop | why it is a gap |
|---|---|---|
| cpu/access-time, cpu/io-access-bitwidth | load at BFC00000 | reads the BIOS ROM; no BIOS image is part of the model (HLE BIOS) |
| cpu/code-in-io | indirect call to 1F800000 | executes code from the scratchpad; the interpreter tier does not run from the scratchpad |
| cpu/cop, spu/test | guest exception 9 at an illegal opcode | the tests provoke reserved-instruction/coprocessor exceptions; the exception layer aborts (docs 04) |

## Full table (3000 ticks each)
| program | result | last message | features touched (group:distinct ids) |
|---|---|---|---|
| cdrom/disc-swap/disc-swap.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010560 ra=800100f8] | bios:6, gp0:5, gp1:7, cd_cmd:3, dma_chan:1, io_read:5, io_write:9, irq:2, exception:1 |
| cdrom/getloc/getloc.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010f30 ra=80010f18] | bios:6, gp0:5, gp1:7, cd_cmd:10, dma_chan:1, io_read:6, io_write:9, irq:2, exception:1 |
| cdrom/terminal/terminal.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011270 ra=8001126c] | bios:11, gp0:5, gp1:7, dma_chan:1, io_read:4, io_write:10, irq:1, exception:1 |
| cdrom/timing/timing.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=800109e8 ra=80010484] | bios:6, gp0:5, gp1:7, cd_cmd:7, dma_chan:1, io_read:6, io_write:10, irq:2, exception:1, cd_sector:1 |
| cpu/access-time/access-time.exe | FATAL | PS1 FATAL: load at bfc00000 (physical 1fc00000): unmapped or unproven region (expansion 1/3, BIOS ROM) [last recorded pc=80010580 ra=80010570] | bios:5, gp0:5, gp1:7, dma_chan:1, io_read:4, io_write:7, exception:1 |
| cpu/code-in-io/code-in-io.exe | FATAL | PS1 FATAL: indirect call to 1f800000: no generated function or HLE hook is registered there [last recorded pc=800101c4 ra=800101cc] | bios:6, gp0:5, gp1:7, dma_chan:1, io_read:3, io_write:6, exception:1, interp_entry:1 |
| cpu/cop/cop.exe | FATAL | PS1 FATAL: guest exception 9 (break) at pc=80010144 arg=fffff: no handler is implemented (BIOS layer is queue P3-1) [last recorded pc=80010138 ra=80010144] | bios:5, gp0:5, gp1:7, dma_chan:1, io_read:3, io_write:6, exception:2 |
| cpu/io-access-bitwidth/io-access-bitwidth.exe | FATAL | PS1 FATAL: load at bfc00000 (physical 1fc00000): unmapped or unproven region (expansion 1/3, BIOS ROM) [last recorded pc=80010ca0 ra=80010c98] | bios:5, gp0:5, gp1:7, dma_chan:1, io_read:3, io_write:7, exception:1 |
| dma/chain-looping/chain-looping.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011034 ra=80010f50] | bios:6, gp0:5, gp1:7, dma_chan:1, io_read:6, io_write:7, irq:2, exception:1 |
| dma/chopping/chopping.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=800109cc ra=8001023c] | bios:6, gp0:6, gp1:8, dma_chan:1, io_read:5, io_write:7, irq:1, exception:1 |
| dma/dpcr/dpcr.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=8001115c ra=80011078] | bios:6, gp0:6, gp1:7, dma_chan:2, io_read:7, io_write:64, irq:1, exception:1 |
| dma/otc-test/otc-test.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=800129d8 ra=800128f4] | bios:6, gp0:5, gp1:7, dma_chan:2, io_read:6, io_write:8, irq:1, exception:1 |
| gpu/animated-triangle/animated-triangle.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010cf4 ra=80010c10] | bios:6, gp0:6, gp1:7, dma_chan:2, io_read:4, io_write:8, irq:1, exception:1 |
| gpu/bandwidth/bandwidth.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=8001137c ra=80011298] | bios:6, gp0:16, gp1:7, dma_chan:1, io_read:5, io_write:6, irq:1, exception:1, interp_entry:1 |
| gpu/benchmark/benchmark.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80015108 ra=80015024] | bios:8, gp0:10, gp1:7, dma_chan:2, io_read:4, io_write:8, irq:1, exception:1 |
| gpu/clipping/clipping.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010dec ra=80010d08] | bios:6, gp0:7, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/clut-cache/clut-cache.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=8001195c ra=80011878] | bios:6, gp0:9, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/gp0-e1/gp0-e1.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=800112b8 ra=800111d4] | bios:6, gp0:7, gp1:8, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/lines/lines.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010f7c ra=80010e98] | bios:6, gp0:7, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/mask-bit/mask-bit.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010f14 ra=80010e30] | bios:6, gp0:8, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/quad/quad.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010ba0 ra=80010abc] | bios:6, gp0:6, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/rectangles/rectangles.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010ec8 ra=80010de4] | bios:6, gp0:39, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/texture-flip/texture-flip.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010cc8 ra=80010be4] | bios:6, gp0:8, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/texture-overflow/texture-overflow.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010c24 ra=80010b40] | bios:6, gp0:8, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/transparency/transparency.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010ad0 ra=800109ec] | bios:6, gp0:6, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/triangle/triangle.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010ae0 ra=800109fc] | bios:6, gp0:6, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/uv-interpolation/uv-interpolation.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010c28 ra=80010b44] | bios:6, gp0:8, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gpu/version-detect/version-detect.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010864 ra=80010780] | bios:6, gp0:4, gp1:8, dma_chan:1, io_read:3, io_write:6, irq:1, exception:1 |
| gpu/vram-to-vram-overlap/vram-to-vram-overlap.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011334 ra=80011250] | bios:6, gp0:11, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gte-fuzz/gte-fuzz.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011d4c ra=80011c68] | bios:8, gp0:8, gp1:7, dma_chan:1, io_read:4, io_write:6, irq:1, exception:1 |
| gte/test-all/test-all.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011468 ra=80010080] | bios:7, gp0:4, gp1:7, dma_chan:1, io_read:3, io_write:6, irq:1, exception:1, interp_entry:1 |
| input/pad/pad.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010b3c ra=80010a58] | bios:8, gp0:4, gp1:7, dma_chan:1, io_read:3, io_write:6, irq:1, exception:1 |
| mdec/4bit/4bit.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011418 ra=80011334] | bios:6, gp0:6, gp1:7, dma_chan:3, mdec_cmd:3, io_read:8, io_write:11, irq:1, exception:1 |
| mdec/8bit/8bit.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011408 ra=80011324] | bios:6, gp0:6, gp1:7, dma_chan:3, mdec_cmd:3, io_read:8, io_write:11, irq:1, exception:1 |
| mdec/frame/frame-15bit-dma.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011140 ra=8001105c] | bios:6, gp0:7, gp1:7, dma_chan:3, mdec_cmd:3, io_read:8, io_write:11, irq:1, exception:1 |
| mdec/frame/frame-15bit.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010178 ra=800108f8] | bios:6, gp0:7, gp1:7, dma_chan:2, mdec_cmd:3, io_read:6, io_write:9, irq:1, exception:1 |
| mdec/frame/frame-24bit-dma.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011154 ra=80011070] | bios:6, gp0:7, gp1:7, dma_chan:3, mdec_cmd:3, io_read:8, io_write:11, irq:1, exception:1 |
| mdec/frame/frame-24bit.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=8001018c ra=8001090c] | bios:6, gp0:7, gp1:7, dma_chan:2, mdec_cmd:3, io_read:6, io_write:9, irq:1, exception:1 |
| mdec/movie/movie-15bit.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011158 ra=80011074] | bios:6, gp0:6, gp1:7, dma_chan:3, mdec_cmd:3, io_read:8, io_write:11, irq:1, exception:1 |
| mdec/movie/movie-24bit.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011178 ra=80011094] | bios:6, gp0:6, gp1:7, dma_chan:3, mdec_cmd:3, io_read:8, io_write:11, irq:1, exception:1 |
| mdec/step-by-step-log/step-by-step-log.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80011520 ra=8001143c] | bios:6, gp0:7, gp1:7, dma_chan:1, mdec_cmd:3, io_read:6, io_write:9, irq:1, exception:1 |
| spu/memory-transfer/memory-transfer.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=800129d8 ra=800128f4] | bios:6, gp0:6, gp1:7, dma_chan:2, io_read:8, io_write:65, irq:1, exception:1 |
| spu/ram-sandbox/ram-sandbox.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=8001209c ra=80011fb8] | bios:8, gp0:7, gp1:7, dma_chan:2, io_read:6, io_write:63, irq:1, exception:1 |
| spu/stereo/stereo.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010d34 ra=80010c50] | bios:6, gp0:4, gp1:7, dma_chan:2, spu_keyon_voice:2, io_read:5, io_write:63, irq:1, exception:1 |
| spu/test/test.exe | FATAL | PS1 FATAL: guest exception 9 (break) at pc=8001015c arg=fffff: no handler is implemented (BIOS layer is queue P3-1) [last recorded pc=80010bb8 ra=8001015c] | bios:6, gp0:4, gp1:7, dma_chan:1, io_read:3, io_write:7, irq:1, exception:2 |
| spu/toolbox/toolbox.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=800122a8 ra=80011150] | bios:8, gp0:8, gp1:7, dma_chan:3, io_read:62, io_write:75, irq:1, exception:1 |
| timer-dump/timer-dump.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=80010c2c ra=80010b48] | bios:6, gp0:4, gp1:7, dma_chan:1, io_read:5, io_write:9, irq:1, exception:1 |
| timers/timers.exe | ok | PS1 FATAL: tick limit reached [last recorded pc=8001159c ra=800114b8] | bios:7, gp0:4, gp1:7, dma_chan:1, io_read:5, io_write:10, irq:1, exception:1, interp_entry:1 |
