# 04 — BIOS layer (HLE) a recompiled program needs

Entry: programs call the kernel by jumping to `0xA0/0xB0/0xC0` with the function number in `t1`. Route those jumps in the runtime (not through
the BIOS ROM, which a port does not have).

## Implemented and proven (observed to be reached by five executables)
- **Events** (B 07h..0Dh, 20h): `OpenEvent(class, spec, mode, func)` returns `0xF1000000 + slot`; of the default 16 slots **five belong to the BIOS's CD code** (class `F0000003`, specs `10h 20h 40h 80h 8000h`, psx-spx) and exist from the start in slots 0..4;
  `EnableEvent/DisableEvent/CloseEvent/TestEvent/WaitEvent/DeliverEvent/UndeliverEvent`. Mode `0x1000` = run the callback, `0x2000` = set
  "ready" (polled by Test/WaitEvent). `WaitEvent` semantics (as in the independent HLE kernel): status *ready* → set active and return 1; status *not active* (closed/disabled/unknown) → return 0 immediately; status *active* → block. It must let virtual time pass (it spins calling the frame hook), otherwise nothing can ever deliver the event.
- Event classes (psx-spx list): `F0000001` vblank, `F0000008` joypad IRQ7, `F0000009` SPU IRQ9, `F2000000..03` root counters (3 = software vblank
  timer), `F4000001` memory-card high-level, `F0000011` memory-card sector level. The kernel alone delivers a few of them; **libraries also call
  `DeliverEvent` themselves** (observed: an SPU DMA-completion callback delivers `F0000009/0020` to wake a `WaitEvent`).
- **Exception exit**: `SetCustomExitFromException(addr)` (B 19h) points at a SaveState-format buffer (`+0 pc, +4 sp, +8 fp, +0xC..+0x28 r16..r23,
  +0x2C gp`); the interrupt hook runs with `r2 = 1`, ends with `ReturnFromException` (B 17h). The registers other than those in the buffer are the
  interrupted code's: **callbacks see leftover a0..a3**; event callbacks called from HLE should get explicit values (we pass zeros; psx-spx does
  not document them, and the C runtime and the oracle must agree).
- **Critical sections**: `EnterCriticalSection/ExitCriticalSection` library functions and the inline `syscall 1/2` (see 01) toggle `SR.IE`.
- **DoExecute (A 43h) / Exec**: see 03; returns when the called program returns. The called program may be an overlay (relocated by the caller).
- **GPU helpers** A 48h/49h/4Ah/4Dh/4Eh forward to the GPU registers (`gpu_sync` never waits when transfers are instantaneous).
- **Misc**: A 39h InitHeap, A 44h FlushCache (no-op: no instruction cache), A 72h CdRemove, B 14h..16h pad init/read, B 5Bh ChangeClearPad,
  C 0Ah ChangeClearRCnt, B 35h/36h file write (stdout capture) — all as no-op or flag contracts taken from psx-spx.
- **Memory-card calls**: see 11. Completion events are *deferred to the next frame*: delivering them inside the call, before the program enabled its
  callbacks, lost them.
- **`A(72h)` CdRemove** closes the five stored CD-event handles (`F1000000..F1000004`) *every time it is called*, whatever event occupies those slots by then. Programs call it from their callback-library reset; after an earlier CdRemove freed the slots, later program events (e.g. an SPU-transfer event) are allocated there, and a second CdRemove closes them. `WaitEvent` on a closed/unknown handle returns 0 at once. Modelling this made a program continue past a point where it otherwise waited forever (inferred real behaviour; see 14). **Documented** (16, C1): an independent open-source BIOS opens exactly these five CD events (class `F0000003`, specs `10h 20h 40h 80h 8000h`, in that order, first-free-slot allocation) and closes the five stored handles when the CD code is shut down; its `WaitEvent` returns 1 on ready, blocks when enabled and returns 0 otherwise. This agrees with the model above, but it is a re-implementation, not the retail BIOS.
- Unimplemented calls abort naming `A/B/C(n)`.

## Pad
The pad BIOS call returns a 32-bit word: pad 1 in the low half, active low, byte order reversed (psx-spx). A frontend sets the word per frame; a
virtual-frame script (`button@first-last`) is enough for tests. A vblank hook stores the word into the "button destination" the program registered.
The kernel vblank event for the pad must exist (`F2000003/2`): without it the program never saw input.

## Unknown
Real kernel register state at `DoExecute` (SR after boot was chosen `0x401`: IE + IM2); exact kernel register values at callback entry.

Origin: f2b#0022, 0023, 0027.
