# Provisional pieces (stubs / hooks / assumptions)

Everything that replaces real code or is a guess, with its evidence. Kept in sync with `game/hooks.csv` by `tools/check_stubs.py`.

| Where | What | Why it is safe / what is unknown |
|---|---|---|
| (add one row per title-specific stub: the hook table row, the low-RAM range, ... with its evidence) | | |
| hook `Null` | SDK routines that patch the BIOS in RAM (table lookups, exception-handler words) | nothing to patch in a HLE BIOS; audit each routine before binding (docs 04 of AnyPS1) |
| hooks `EnterCriticalSection`, `ExitCriticalSection` | `syscall` wrappers | single-threaded port, interrupts masked through COP0 SR |
| BIOS C library: `strcmp`, `strncmp`, `bcopy`, `bzero`, `SetJmp`, `printf`, memchr/memcpy/memset/strcpy/strcat/strlen/rand/srand | `tools/recomp/biosc.py`, `runtime/biosc.inc` | contracts from psx-spx; return values the documentation does not give are inferred (docs 04) |

## BIOS calls and hardware placeholders carried over from the first two ports (platform level; same contracts)

| What | Where | Evidence | Note |
|---|---|---|---|
| host function | guest address/BIOS call | finding | notes |
| `gte_mfc2/cfc2/mtc2/ctc2/gte_cmd` (`runtime/sys.c`) → `gte_core_*` (`runtime/gte.c`) | COP2 instructions | 0024 | **implemented** (all psx-spx commands, verified against an independent reference); a command code outside the psx-spx list still aborts; no GTE timing |
| `rt_exception` (`runtime/sys.c`) | syscall / break / overflow | 0018 | **aborts**; the BIOS exception layer is queue P3-1 |
| `InitHeap` (A(39h)) hook, `runtime/hle.c`, `tools/recomp/hooks.py` | `<addr>` the executable | 0019 | no-op: no BIOS call that consumes the heap is used by the executable (finding 0006); revisit if another executable uses malloc-family calls |
| `FileWrite` hook | `<addr>` the executable | 0019 | only fd 1 (captured to host buffer); other fds abort |
| `DoExecute` hook | `<addr>` the executable | 0019, 0025 | single-executable test runs record the arguments and stop; the whole-game build (`run_game`) really loads and runs the next recompiled executable (zero-fill, stack, GP, return value 1 as in psx-spx A(43h)) |
| BIOS B(15h) OutdatedPadInitAndStart, B(16h) OutdatedPadGetButtons, B(14h) StopPad | pad init stubs | 0021, psx-spx | semantics from psx-spx; the button word comes from the pad provider (`rt_pad_set`, scripted presses, the SDL2 player, random input of `--fuzz`); default `0xFFFFFFFF` = no buttons |
| BIOS B(5Bh) ChangeClearPad, C(0Ah) ChangeClearRCnt, A(44h) FlushCache, B(35h) FileWrite | stubs | 0021, psx-spx | flags are kept (initial value 0 is an assumption, psx-spx gives none); FlushCache is a no-op because no instruction cache exists and no code is written at run time (finding 0014) |
| Every other BIOS call stub | all | 0021 | abort naming the call (`BIOS X(NN) is not implemented`) |
| BIOS A(39h) InitHeap, A(44h) FlushCache, B(14h) StopPad | BIOS | 0022, psx-spx | no-ops: no heap-consuming BIOS call is used, no instruction cache exists, the BIOS cdrom IRQ handler does not exist, no pad IRQ handler exists |
| Timers 0..2 (`runtime/io.c`) | `0x1f801100..0x1f80112f` | 0022 | registers; timer 1 counts scanlines per virtual frame; timer 2 follows its mode register and raises its interrupt (finding 0040); timer 0 does not move and timers 0/1 raise no interrupts |
| BIOS kernel IRQ handlers | vblank, CD | 0022, 0025 | the kernel's interrupt-handler chain is not run: at every virtual frame the model raises I_STAT (vblank, CD INT1..5) and the game's own handler is called through the custom exit; `F0000001h`/2 and `F2000003h`/2 events are delivered per frame; DMA, SPU, timer and pad events are never delivered (psx-spx classes) |
| BIOS events: OpenEvent, CloseEvent, EnableEvent, DisableEvent, TestEvent, WaitEvent, DeliverEvent, UnDeliverEvent | B(07h..0Dh, 20h) | 0022, psx-spx | real semantics from psx-spx; only vblank (`F0000001h`/2) and memory-card events are delivered by the host; WaitEvent lets virtual time pass (fatal after 100 000 frames) |
| BIOS exception exit: SetCustomExit, SetDefaultExit, ReturnFromException | B(19h), B(18h), B(17h) | 0022, psx-spx | the BIOS kernel's own interrupt-handler chain is absent: the custom exit runs directly after a virtual vblank |
| BIOS GPU: SendGP1Command, GPU_cw, GPU_cwp, GetGPUStatus, gpu_sync | A(48h), A(49h), A(4Ah), A(4Dh), A(4Eh) | 0022, psx-spx | forward to the register model; `gpu_sync` never waits (transfers are instantaneous) |
| `card.c` virtual memory card + BIOS card/file calls | B(32h..36h, 41h..45h, 4Ah..50h, 5Ch, 5Dh, 54h, 55h), A(55h, 70h, ABh..ADh) | 0023 | file-backed 128 KiB card in the psx-spx format; completion events are delivered at once; the card-changed flag, broken sectors and exact error numbers are not modelled |
| pad provider `rt_pad_set` / `Hle.pad_value` | B(15h), B(16h) | 0023 | the frontend (queue P5-3) supplies the button word; default no buttons |
| SPU (`runtime/spu.inc`, `io.c`) | `0x1f801c00..0x1f801e5f`, DMA 4 | 0025 | 24 voices, ADPCM, Gaussian interpolation, ADSR, noise, reverb, XA input, 44.1 kHz output ring; **not modelled**: SPU IRQ address (register is storage), sweep volumes (end value is used), capture buffers, the 25-point XA resampler (linear), XA 8 bit (ignored), SPUSTAT beyond the mode bits, exact transfer timing (DMA/manual writes are instantaneous) |
| GPU rasteriser (`runtime/io.c`) | `0x1f801810/14`, DMA 2 | 0025 | software renderer: plane-gradient fixed-point interpolation of colour and UV with the leftmost vertex as origin, fixed-point line stepping and poly-lines, mask bit, texture-disable bit, row-buffered VRAM copies, the CLUT cache (findings 0032, 0043); pixel-identical to the public ps1-tests captures on 9 tests and 100.00 % to the reference emulator on game frames; **not modelled**: drawing time (instantaneous), the 1/16 sub-pixel details beyond what those tests exercise |
| MDEC (`runtime/io.c`) | `0x1f801820/24`, DMA 0/1 | 0025 | exact decoder (300 random streams identical to an independent implementation, finding 0028), instantaneous |
| CD-ROM controller (`runtime/io.c`) | `0x1f801800..03` | 0025 | the commands the game uses are modelled (list in finding 0025); every other command answers INT5(stat|1, 40h); seek/rotation delays are constants (2 ticks); data rate 1.25 / 2.5 sectors per 60 Hz tick; disc is one raw track (GetTN/GetTD answer a single data track); GetlocP is an approximation |
| BIOS event callbacks (`guest_call`, `Oracle.call_guest`) | EVENT class callbacks | 0026 | called with a0..a3 = 0 (undocumented in psx-spx) |
| BIOS A(72h) CdRemove + the BIOS's five CD events | A(72h); event slots 0..4 | 0030 | the cdrom IRQ handler does not exist (nothing to remove); the five CD events (class F0000003h, specs 10h 20h 40h 80h 8000h) exist at start and CdRemove closes the stored handles F1000000h..04 every call (psx-spx documents 5 BIOS CD events; the handle-variable lifecycle is inferred, finding 0030) |
| SysEnqIntRP, SysDeqIntRP, memchr, memcpy, memset, putchar, puts, srand, strcat, strcpy, strlen | C(02h) C(03h), A(2Eh) A(2Ah) A(2Bh), B(3Dh) A(3Ch) B(3Fh) A(3Eh), A(30h), A(15h) A(19h) A(1Bh) | finding 0001, psx-spx | BIOS C-library twin (`tools/recomp/biosc.py`, `runtime/biosc.inc`); interrupt chains are bookkeeping only (finding 0003) |
| kernel device control block table in low RAM (0x6EE0, one entry "bu") | `[0x150]`, `[0x154]`, 0x6EE0..0x7200, 0xEFF0 | 0015 | the game's libcard wrapper (<addr>) walks the BIOS device table to find the "bu" device (and patches a function slot of it) before it calls B(42h) firstfile; without the table it finds no device and the Load screen says "no data". Only the first entry exists (name pointer, name "bu") exactly as in the reference BIOS; the other fields stay zero because our HLE firstfile/read/write never use them |
| io.c, gte.c, card.c | hardware placeholders | findings 0002, 0003 | register-level models carried over from the first two ports; DMA is instantaneous with a queued completion interrupt |

