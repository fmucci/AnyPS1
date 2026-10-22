# 17 — Controller port (SIO0) and time inside a frame

Game-independent notes from bringing up an SDK whose pad driver talks to the serial port registers directly (not through the BIOS pad calls).

## Registers (psx-spx "Serial Port (SIO)")
DATA 1F801040h (RX FIFO / TX), STAT 44h (bit0 TX ready, bit1 RX not empty, bit2 TX idle, bit7 /ACK level, bit9 IRQ flag), MODE 48h, CTRL 4Ah (bit0 TXEN, bit1 DTR = select, bit4 acknowledge, bit6 reset, bit12 ACK-interrupt enable, bit13 port select), BAUD 4Eh. Interrupt = I_STAT bit 7.

## Digital pad protocol (observed with a real SDK driver)
With DTR set on port 1: bytes `01h 42h 00h 00h 00h` are answered `FFh 41h 5Ah <low> <high>` (active-low buttons: low = select l3 r3 start up right down left, high = l2 r2 l1 r1 triangle circle cross square); each byte except the last is acknowledged. Drivers also send `43h 44h 45h 46h 47h 4Ch 4Dh` (DualShock configuration); answering them like a poll (a digital pad ignores them) is enough for the driver to settle on "digital". Port 2 and a memory-card select (first byte 81h) can simply not answer (RX FFh, no ACK). Deselecting (DTR low) ends the transaction.

## The three timing rules that made a polling driver work
1. **The answer must not be instantaneous and must not be late.** The driver writes the byte, clears I_STAT bit 7 and tests STAT a few instructions later; if the ACK bit is already set it is erased, if it appears only at the next virtual frame the driver's wait times out and it aborts the transaction after the first byte. Make the RX byte, STAT bit 9 and I_STAT bit 7 appear after a few STAT reads (or at the frame boundary for interrupt-driven drivers).
2. **Time must pass inside a frame.** SDK waits are "spin until root counter 2 has advanced N counts". A host that moves counters only once per virtual frame turns every wait into at least a frame; give every read of a counter value a small advance (the time of a polling-loop iteration), saturating below the frame length, and restart from the base value at the next frame.
3. **The frame length constant is a calibration.** If virtual time is counted in taken backward branches, a title whose per-frame CPU work is large (software VLC decoding of movies) needs a much larger period than the one that suits a simple game; compare the time to a known screen (title, a movie end) with a reference emulator and tune the constant.

## Kernel interrupt chains
The BIOS exception handler walks the chains registered with `SysEnqIntRP` (priority 0..3, entry `{+0 next, +4 handler, +8 test}`): call the test function, if it returns non-zero call the handler. An SDK that puts its pad/card poll in such an entry needs this walk; keeping only the queue leaves it dead.
