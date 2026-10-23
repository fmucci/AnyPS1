"""HLE hooks for the oracle (queue P2-3). The C versions live in runtime/hle.c and MUST implement the same contract; the traces compare them.

A hook replaces guest code at a guest address. Hooks are bound to an executable by (address, masked-signature hash): `bind()` refuses
to bind when the first words of the function at that address do not match the committed signature (decisions/0002 section 3.5).
Contracts are PROVISIONAL, marked `prov`: they reproduce only the guest-visible effect the launcher path relies on (derived from the guest
disassembly in finding 0019); their fidelity to the real library is the subject of queue P3-1/P3-2 and each is listed in docs/stubs.md.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.lib_match import mask  # noqa: E402
from tools.recomp import disc as discmod  # noqa: E402
from tools.recomp import gamecfg  # noqa: E402
from tools.recomp.oracle import Fatal, IrqReturn, StopRun  # noqa: E402
from tools.recomp import oracle as oracle_mod  # noqa: E402

SIG_WORDS = 12


def signature(words_at):
    """sha256 over the masked first SIG_WORDS instruction words (relocation-independent), hex prefix 16."""
    h = hashlib.sha256()
    for w in words_at:
        h.update(struct.pack("<I", mask(w)))
    return h.hexdigest()[:16]


from tools.recomp.biosc import LibC  # noqa: E402


class Hle(LibC):
    """Hook implementations (by name) shared by all executables. State: current CD position set by Setloc, captured stdout, DoExecute calls."""

    def __init__(self, disc_path):
        self.disc = discmod.Disc(disc_path)
        self.loc = None
        self.stdout = bytearray()
        self.execs = []
        self.ie = 1
        self.param = 0
        self.param2 = 0
        self.pad_dest = 0
        self.pad_value = 0xFFFFFFFF
        self.pad_script = []        # [(first vblank, last vblank, pad word)]: input for tests and demos
        self.vblanks = 0
        self.chain = False          # True: DoExecute really runs the next executable (tools/recomp/run_oracle.py --chain)
        self.last_file = None
        self.current = None
        self.disc_dir = gamecfg.DISC_DIR
        self.max_steps_per_program = 2_000_000_000
        self.card = None            # set by the caller: ctypes handle from oracle.io.card_new (None = no card layer)
        self.events = [None] * 16
        for i, sp in enumerate((0x10, 0x20, 0x40, 0x80, 0x8000)):        # the BIOS's own CD events (see EV_FIRST note)
            self.events[i] = {"class": 0xF0000003, "spec": sp, "mode": 0x2000, "func": 0, "status": 1}
        self.pending_events = []   # asynchronous BIOS events (memory card) delivered at the next virtual frame
        self.clear_pad = 0
        self.clear_rcnt = [1, 1, 1, 0]       # twin of g_clear_rcnt in runtime/hle.c

    # ---- contract implementations; each gets the oracle, returns None (= return to $ra) ----
    def null_hle(self, o):                        # provisional null hook (docs/stubs.md): logs once per address on stderr, returns v0 = param (default 0)
        r = o.st.r
        o.st.r[2] = self.param

    # ---- BIOS calls (stub-level hooks); implemented ones are listed in BIOS_NAMES ----
    def vsync(self, o):
        """libgpu/libetc VSync(mode) (CREDITS 0x8002dc44; contract read from its disassembly, finding 0022). param = frame-counter address,
        param2 = address of the library's state block {+0 &GPUSTAT, +4 &timer1 value, +8 last timer1, +0xC last frame}. Time is host-owned: each
        wait advances the virtual vblank counter (guest variable at param) and the model's timer 1 by one frame."""
        r = o.st.r
        mode = r[4] - (1 << 32) if r[4] & 0x80000000 else r[4]
        fc_addr, st = self.param, self.param2
        fc = o.mem.load(fc_addr, 4)
        last_rc = o.mem.load(st + 8, 4)
        rc = o.hw_read(o.mem.load(st + 4, 4), 4)
        hc = (rc - last_rc) & 0xFFFF
        if mode < 0:
            r[2] = fc
            return
        if mode == 1:
            r[2] = hc
            return
        last_frame = o.mem.load(st + 0xC, 4)
        target = last_frame - 1 + mode if mode > 0 else last_frame
        while fc < target:
            o.io.io_vblank_tick(o.io_ctx)
            fc += 1
        o.io.io_vblank_tick(o.io_ctx)
        fc += 1
        self._store_quiet(o, fc_addr, fc)
        self._store_quiet(o, st + 0xC, fc)
        self._store_quiet(o, st + 8, o.hw_read(0x1F801110, 4))
        r[2] = hc

    @staticmethod
    def _store_quiet(o, addr, v):
        o.mem.quiet = True
        o.mem.store(addr, 4, v)
        o.mem.quiet = False

    BIOS_NAMES = {("B", 0x07): "DeliverEvent", ("B", 0x08): "OpenEvent", ("B", 0x09): "CloseEvent", ("B", 0x0A): "WaitEvent", ("B", 0x0B): "TestEvent",
                  ("B", 0x0C): "EnableEvent", ("B", 0x0D): "DisableEvent", ("B", 0x20): "UnDeliverEvent", ("A", 0x72): "CdRemove", ("B", 0x17): "ReturnFromException", ("B", 0x18): "SetDefaultExit", ("B", 0x19): "SetCustomExit", ("A", 0x39): "InitHeap", ("A", 0x43): "DoExecute", ("B", 0x15): "OutdatedPadInitAndStart", ("B", 0x16): "OutdatedPadGetButtons", ("B", 0x14): "StopPad", ("B", 0x5B): "ChangeClearPad",
                  ("C", 0x0A): "ChangeClearRCnt", ("A", 0x44): "FlushCache", ("B", 0x35): "FileWrite",
                  ("A", 0x48): "SendGP1Command", ("A", 0x49): "GPU_cw", ("A", 0x4A): "GPU_cwp", ("A", 0x4D): "GetGPUStatus", ("A", 0x4E): "gpu_sync"}
    BIOS_NAMES.update(LibC.NAMES)
    PAD_NONE = 0xFFFFFFFF    # "no buttons pressed / pad disconnected" (psx-spx B(16h): halfword FFFFh)
    # the input provider sets `pad_value` (32 bit word as B(16h) returns it: pad 1 in the low halfword, active low, reversed byte order)

    def bios_OutdatedPadInitAndStart(self, o):    # psx-spx B(15h): fails (0) unless type is 20000000h/20000001h, else memorizes button_dest, returns 2
        r = o.st.r
        if r[4] not in (0x20000000, 0x20000001):
            r[2] = 0
            return
        self.pad_dest = r[5]
        if self.pad_dest:
            o.mem.quiet = True
            o.mem.store(self.pad_dest, 4, self.pad_value)
            o.mem.quiet = False
        r[2] = 2

    def bios_OutdatedPadGetButtons(self, o):      # psx-spx B(16h): returns the buttons (reversed byte order) and stores them at [button_dest]
        o.st.r[2] = self.pad_value
        if self.pad_dest:
            o.mem.quiet = True
            o.mem.store(self.pad_dest, 4, self.pad_value)
            o.mem.quiet = False

    def bios_ChangeClearPad(self, o):             # psx-spx B(5Bh): selects whether the pad/card IRQ handler auto-acknowledges; no IRQs are delivered yet, flag kept
        self.clear_pad = o.st.r[4]

    def bios_ChangeClearRCnt(self, o):            # psx-spx C(0Ah): sets the auto-ack flag of timer t (0..2) or vblank (3), returns the OLD flag (initial value 0: not stated by psx-spx)
        t, flag = o.st.r[4], o.st.r[5]
        if t > 3:
            raise Fatal("ChangeClearRCnt with t=%d" % t)
        o.st.r[2] = self.clear_rcnt[t]
        self.clear_rcnt[t] = flag

    def bios_FileWrite(self, o):                  # psx-spx B(35h) FileWrite(fd,src,length), same contract as the launcher hook
        return self.file_write(o)

    # ---- BIOS GPU functions (psx-spx "BIOS GPU Functions"); transfers are instantaneous so waits are no-ops ----
    @staticmethod
    def _gpu_sync(o):
        if (o.hw_read(0x1F801814, 4) >> 29) & 3:      # DMA direction on: D2_CHCR.24 is already 0, then turn DMA off via GP1(04h)
            o.hw_write(0x1F801814, 4, 0x04000000)
        return 0

    def bios_gpu_sync(self, o):
        o.st.r[2] = self._gpu_sync(o)

    def bios_SendGP1Command(self, o):
        o.hw_write(0x1F801814, 4, o.st.r[4])

    def bios_GPU_cw(self, o):
        o.st.r[2] = self._gpu_sync(o)
        o.hw_write(0x1F801810, 4, o.st.r[4])

    def bios_GPU_cwp(self, o):
        src, n = o.st.r[4], o.st.r[5]
        self._gpu_sync(o)
        for k in range(n):
            o.hw_write(0x1F801810, 4, o.mem.load(src + 4 * k, 4))
        o.st.r[2] = 0

    def bios_GetGPUStatus(self, o):
        o.st.r[2] = o.hw_read(0x1F801814, 4)

    def bios_FlushCache(self, o):                 # psx-spx A(44h): the port has no instruction cache; code is never changed at run time (finding 0014)
        return None

    def bios_StopPad(self, o):                    # psx-spx B(14h): stops the pad/card IRQ handler; nothing is observable in the port
        return None

    def bios_ReturnFromException(self, o):        # psx-spx B(17h): resumes the interrupted code; here: leaves the nested handler run
        if not o.in_irq:
            raise Fatal("ReturnFromException outside an interrupt")
        raise IrqReturn()

    def bios_SetDefaultExit(self, o):             # psx-spx B(18h)
        o.custom_exit = 0

    def bios_SetCustomExit(self, o):              # psx-spx B(19h): address of the library's interrupt handler
        o.custom_exit = o.st.r[4]

    # ---- BIOS events (psx-spx "BIOS Event Functions"). Descriptor = 0xF1000000 + slot; slots 0..4 start as the BIOS's five CD events (class F0000003, specs 10h 20h 40h 80h 8000h, psx-spx: "5 events are internally used by the BIOS for CDROM functions"); A(72h) CdRemove closes the five stored handles F1000000..F1000004 each time it is called, whatever occupies those slots then (finding 0030) ----
    EV_FIRST, EV_COUNT = 5, 16
    EV_BUSY, EV_READY = 1, 2             # status of an enabled event; 0 = disabled

    def _ev(self, h):
        i = h - 0xF1000000
        return self.events[i] if 0 <= i < self.EV_COUNT and self.events[i] else None

    def bios_OpenEvent(self, o):
        r = o.st.r
        for i in range(0, self.EV_COUNT):
            if self.events[i] is None:
                self.events[i] = {"class": r[4], "spec": r[5], "mode": r[6], "func": r[7], "status": 0}
                r[2] = 0xF1000000 + i
                return
        r[2] = 0xFFFFFFFF

    def bios_CloseEvent(self, o):
        e = self._ev(o.st.r[4])
        if e:
            self.events[o.st.r[4] - 0xF1000000] = None
        o.st.r[2] = 1

    def bios_EnableEvent(self, o):
        e = self._ev(o.st.r[4])
        if e:
            e["status"] = self.EV_BUSY
        o.st.r[2] = 1

    def bios_DisableEvent(self, o):
        e = self._ev(o.st.r[4])
        if e:
            e["status"] = 0
        o.st.r[2] = 1

    def bios_TestEvent(self, o):
        e = self._ev(o.st.r[4])
        if e and e["status"] == self.EV_READY:
            e["status"] = self.EV_BUSY
            o.st.r[2] = 1
        else:
            o.st.r[2] = 0

    def bios_WaitEvent(self, o):                  # single thread: let virtual time pass until the event is ready
        h = o.st.r[4]
        e = self._ev(h)
        if not e or e["status"] == 0:
            o.st.r[2] = 0
            return
        for _ in range(100000):
            if e["status"] == self.EV_READY:
                e["status"] = self.EV_BUSY
                o.st.r[2] = 1
                return
            o.tick()
        raise Fatal("WaitEvent(%08x) never became ready" % h)

    EXC_STACK_TOP = 0x80000FF0
    async_events = 0                 # >0 while the virtual-frame events are delivered: callbacks run on the exception stack (finding 0008), like g_async_events in runtime/hle.c

    def deliver_event(self, o, cls, spec):
        for e in self.events:
            if e and e["status"] == self.EV_BUSY and e["class"] == cls and e["spec"] == spec:
                if e["mode"] == 0x1000:
                    if e["func"]:
                        sp = o.st.r[29]
                        if self.async_events > 0 and not (0x80000800 <= sp <= self.EXC_STACK_TOP):
                            o.st.r[29] = self.EXC_STACK_TOP
                        try:
                            o.call_guest(e["func"])
                        finally:
                            o.st.r[29] = sp
                else:
                    e["status"] = self.EV_READY

    def bios_DeliverEvent(self, o):
        self.deliver_event(o, o.st.r[4], o.st.r[5])

    def bios_UnDeliverEvent(self, o):
        for e in self.events:
            if e and e["status"] == self.EV_READY and e["mode"] == 0x2000 and e["class"] == o.st.r[4] and e["spec"] == o.st.r[5]:
                e["status"] = self.EV_BUSY

    PAD_BITS = {"select": 8, "l3": 9, "r3": 10, "start": 11, "up": 12, "right": 13, "down": 14, "left": 15,
                "l2": 0, "r2": 1, "l1": 2, "r1": 3, "triangle": 4, "circle": 5, "cross": 6, "square": 7}   # B(16h) word, active low (psx-spx: reversed byte order)

    @classmethod
    def pad_word(cls, *buttons):
        w = 0xFFFFFFFF
        for b in buttons:
            w &= ~(1 << cls.PAD_BITS[b]) & 0xFFFFFFFF
        return w

    def on_vblank(self, o):                       # kernel VBLANK interrupt: DeliverEvent(F0000001h, 0002h "interrupted") (psx-spx event class list)
        while self.pending_events:
            cls, spec = self.pending_events.pop(0)
            self.deliver_event(o, cls, spec)
        self.vblanks += 1
        for a, b, w in self.pad_script:           # scripted input: (first frame, last frame, pad word)
            if a <= self.vblanks <= b:
                self.pad_value = w
                break
        else:
            if self.pad_script:
                self.pad_value = 0xFFFFFFFF
        if self.pad_dest:                         # the pad/card IRQ handler stores OutdatedPadGetButtons() at [button_dest] (psx-spx B(16h))
            self._store_quiet(o, self.pad_dest, self.pad_value)
        o.io.io_sio_set_pad(o.io_ctx, self.pad_value)         # the controller port the program's own driver talks to
        self.deliver_event(o, 0xF0000001, 0x0002)
        self.deliver_event(o, 0xF2000003, 0x0002)         # root counter 3 = software vblank timer (psx-spx class list; assumed to fire every vblank)

    def on_rcnt_irq(self, o, t):                  # BIOS interrupt handler for root counter t: an enabled source delivers its event (F200000t, spec 2) and is acknowledged when its clear flag is set
        if (o.io.io_imask(o.io_ctx) >> (4 + t)) & 1:
            self.deliver_event(o, 0xF2000000 + t, 0x0002)
            if self.clear_rcnt[t]:
                o.io.io_irq_ack(o.io_ctx, 1 << (4 + t))

    def _async_wrap(f):                           # events delivered from the virtual frame run in the BIOS's exception context (own stack): see deliver_event
        def w(self, o, *a):
            self.async_events += 1
            try:
                return f(self, o, *a)
            finally:
                self.async_events -= 1
        return w
    on_vblank = _async_wrap(on_vblank)
    on_rcnt_irq = _async_wrap(on_rcnt_irq)

    def bios_CdRemove(self, o):                   # psx-spx A(72h): removes the BIOS cdrom IRQ handler (the port has none) and closes the BIOS's five stored CD event handles
        for i in range(5):
            self.events[i] = None

    def card_call(self, o, t, n):
        import ctypes
        r = o.st.r
        a = (ctypes.c_uint32 * 4)(r[4], r[5], r[6], r[7])
        ret = ctypes.c_uint32()
        ev = (ctypes.c_uint32 * 16)()
        nev = ctypes.c_int()
        if not o.io.card_call(self.card, ctypes.addressof(o._ram_c), t.encode(), n, a, ctypes.byref(ret), ev, ctypes.byref(nev)):
            return False
        r[2] = ret.value
        for k in range(nev.value):                  # the card works through IRQs: completion is delivered at the next virtual frame, not inside the call
            self.pending_events.append((ev[2 * k], ev[2 * k + 1]))
        return True

    def bios_InitHeap(self, o):
        return self.init_heap(o)

    def bios_DoExecute(self, o):
        return self.do_execute(o)

    def init_heap(self, o):                       # BIOS A(39h) InitHeap(addr,size): no BIOS call that consumes the heap is used by SLUS (finding 0019)
        return None

    def do_execute(self, o):                      # BIOS A(43h) DoExecute(headerbuf, p1, p2): the harness stops here unless chaining is enabled
        r = o.st.r
        self.execs.append((r[4], r[5], r[6]))
        if not self.chain:
            raise StopRun()
        # psx-spx A(43h): zero-fill the memfill region, SP/FP = base+offset when base != 0, GP from the header, call the entry with p1, p2 in r4/r5;
        # when the executable returns the registers are restored and r2 = 1. The program text is already in guest RAM (the launcher read it).
        hdr, p1, p2 = r[4], r[5], r[6]
        ld = lambda k: o.mem.load(hdr + k, 4)
        entry, gp, fill_a, fill_n, sp_base, sp_off = ld(0), ld(4), ld(0x18), ld(0x1C), ld(0x20), ld(0x24)
        o.mem.quiet = True
        for k in range(fill_n):
            o.mem.store(fill_a + k, 1, 0)
        o.mem.quiet = False
        name = self._handoff_name(o)
        self.switch_exe(o, name)
        saved = (list(r), o.st.hi, o.st.lo)
        r[28] = gp
        if sp_base:
            r[29] = r[30] = (sp_base + sp_off) & 0xFFFFFFFF
        v0 = o.call_guest(entry, p1, p2, 0) if False else self._run_program(o, entry, p1, p2)
        r[:] = saved[0]
        o.st.hi, o.st.lo = saved[1], saved[2]
        r[2] = 1

    def _handoff_name(self, o):                   # SLUS keeps the name of the program it loaded at 0x80010000 (finding 0003)
        b = bytearray()
        while True:
            c = o.mem.load(0x80010000 + len(b), 1)
            if c == 0 or len(b) > 30:
                break
            b.append(c)
        return b.decode("latin1").upper()

    def _run_program(self, o, entry, p1, p2):
        st = o.st
        spec_flush = __import__("tools.recomp.spec", fromlist=["flush"]).flush
        spec_flush(st)
        st.r[4], st.r[5], st.r[6], st.r[7] = p1, p2, 0, 0
        st.r[31] = oracle_mod.CALL_SENTINEL
        st.pc = entry
        res = o.run(self.max_steps_per_program, nested=2)
        if res != "call-returned":
            raise Fatal("program at %08x ended with %s" % (entry, res))
        return st.r[2]

    def switch_exe(self, o, name):                # the next executable replaces the previous one's hooks (all load at 0x80020000)
        from tools.recomp import funcs
        path = os.path.join(self.disc_dir, name)
        img = funcs.Image(path)
        for a in [a for a in o.hooks if a >= 0x00020000]:
            del o.hooks[a]
        o.icache.clear()
        install(o, self, name, img)
        self.current = name

    def file_write(self, o):                      # BIOS B(35h) FileWrite(fd, src, len): fd 1 = host stdout capture
        r = o.st.r
        fd, src, n = r[4], r[5], r[6]
        if fd != 1:
            raise Fatal("FileWrite to fd %d (only stdout is proven used)" % fd)
        for k in range(n):
            self.stdout.append(o.mem.load(src + k, 1))
        r[2] = n

    def intr_init(self, o):                       # libapi IntrInit: returns the address of its state block (per-exe `param`, = the first value the function loads into s0)
        o.st.r[2] = self.param

    def cd_init(self, o):                         # libcd CdInit(mode) -> 1 on success (the guest tests `v0 == 1`)
        o.st.r[2] = 1

    def cd_control(self, o):                      # libcd CdControl(cmd, param, result) -> 1 = command accepted; Setloc(2) / SeekL(0x15) only; result[0] = 0x02 (status: motor on, idle) PROVISIONAL
        r = o.st.r
        cmd, param, result = r[4] & 0xFF, r[5], r[6]
        if cmd not in (2, 0x15, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x1B, 0x06):       # Setloc SeekL Pause Mute Demute Setfilter Setmode (psx-spx CDROM command numbers); filters/modes are recorded by P4-4
            raise Fatal("CdControl command %#x not implemented" % cmd)
        if cmd in (2, 0x15) and param:
            m, s, f = [o.mem.load(param + k, 1) for k in range(3)]
            self.loc = discmod.loc_to_lba(m, s, f)
        if result:
            o.mem.quiet = True
            o.mem.store(result, 1, 0x02)
            o.mem.quiet = False
        r[2] = 1

    def cd_sync(self, o):                         # libcd CdSync(mode, result) -> 2 (CdlComplete): every command completes instantly
        o.st.r[2] = 2

    def cd_control_b(self, o):                    # libcd CdControlB = CdControl + wait for completion (instant here)
        self.cd_control(o)

    def cd_read(self, o):                         # libcd CdRead(sectors, buf, mode) -> 1; synchronous in this library version (guest code at 0x800126a0)
        r = o.st.r
        n, buf, mode = r[4], r[5], r[6]
        if self.loc is None or mode != 0x80:
            raise Fatal("CdRead without Setloc or with mode %#x" % mode)
        data = self.disc.read(self.loc, n)
        o.mem.quiet = True
        for k, b in enumerate(data):
            o.mem.store(buf + k, 1, b)
        o.mem.quiet = False
        r[2] = 1

    def cd_read_sync(self, o):                    # libcd CdReadSync(mode, result) -> 0 sectors remaining
        o.st.r[2] = 0

    def cd_search_file(self, o):                  # libcd CdSearchFile(rec, path) -> rec or 0; 24-byte CdlFILE: loc(4) size(4) name(16)
        r = o.st.r
        rec, p = r[4], r[5]
        s = bytearray()
        while True:
            c = o.mem.load(p + len(s), 1)
            if c == 0:
                break
            s.append(c)
        hit = self.disc.find(s.decode("latin1"))
        if hit:
            self.last_file = hit[2].split(";")[0]
        if hit is None:
            r[2] = 0
            return
        lba, size, name = hit
        m, sec, f = discmod.lba_to_loc(lba)
        blob = bytes([m, sec, f, 0]) + struct.pack("<I", size) + name.encode("latin1")[:16].ljust(16, b"\0")
        o.mem.quiet = True
        for k, b in enumerate(blob):
            o.mem.store(rec + k, 1, b)
        o.mem.quiet = False
        r[2] = rec

    def enter_critical(self, o):                  # syscall 1: v0 = previous IE, IE cleared (psx-spx SYS(01h)); modelled as a host flag
        o.st.r[2] = o.st.cop0[12] & 1
        o.st.cop0[12] &= ~1

    def exit_critical(self, o):                   # syscall 2
        o.st.cop0[12] |= 1




NAMES = {"InitHeap": "init_heap", "DoExecute": "do_execute", "FileWrite": "file_write", "IntrInit": "intr_init", "CdInit": "cd_init",
         "CdControlB": "cd_control_b", "CdRead": "cd_read", "CdReadSync": "cd_read_sync", "CdSearchFile": "cd_search_file",
         "EnterCriticalSection": "enter_critical", "ExitCriticalSection": "exit_critical", "Null": "null_hle", "VSync": "vsync", "CdControl": "cd_control", "CdSync": "cd_sync"}

HOOKS_CSV = gamecfg.HOOKS_CSV


def load_table(exe):
    """Rows of hooks.csv for one executable: [(address, name, param, param2, sig)]. Columns: exe,address,name,param,param2,masked_signature_sha256_prefix."""
    rows = []
    for line in open(HOOKS_CSV).read().splitlines()[1:]:
        e, a, name, param, param2, sig = line.split(",")
        if e == exe:
            rows.append((int(a, 16), name, int(param, 16), int(param2, 16), sig))
    return rows


def install(oracle, hle, exe, img):
    """Bind every hook of `exe` after verifying its masked signature against the image; refuses on any mismatch (decisions/0002 s3.5)."""
    for addr, name, param, param2, sig in load_table(exe):
        i = (addr - img.base) // 4
        got = signature(img.words[i:i + SIG_WORDS])
        if got != sig:
            raise Fatal("hook %s at %08x: masked signature %s != committed %s" % (name, addr, got, sig))
        fn = getattr(hle, NAMES[name])
        if name in ("IntrInit", "Null", "VSync"):
            def f(o, fn=fn, param=param, param2=param2):
                hle.param, hle.param2 = param, param2
                return fn(o)
        else:
            f = fn
        oracle.hook(addr, name, f)


def bios_stubs(img):
    """BIOS call stubs: `li t2,{A0,B0,C0}; jr t2 | jalr ra,t2; li t1,N` (exact 3-word pattern, finding 0006) -> {address: (table, N)}."""
    out = {}
    w = img.words
    for i in range(len(w) - 2):
        x = w[i]
        if (x >> 26) in (0x0D, 0x09) and ((x >> 21) & 31) == 0 and ((x >> 16) & 31) == 10 and (x & 0xFFFF) in (0xA0, 0xB0, 0xC0) \
                and w[i + 1] in (0x01400008, 0x0140F809):
            y = w[i + 2]
            if (y >> 26) in (0x0D, 0x09) and ((y >> 21) & 31) == 0 and ((y >> 16) & 31) == 9:
                out[img.base + 4 * i] = ("ABC"[((x & 0xFFFF) - 0xA0) >> 4], y & 0xFFFF)
    return out


def install_bios(oracle, hle, img=None):
    """Jumps to the BIOS entry points 0xA0/0xB0/0xC0 (t1 = number) run the implemented call or abort naming it (img is unused: kept for callers)."""
    def bios(o, t, n):
        name = hle.BIOS_NAMES.get((t, n))
        impl = getattr(hle, "bios_" + name, None) if name else None
        if hle.card is not None and not (t == "B" and n == 0x35 and o.st.r[4] == 1):     # memory-card layer (runtime/card.c)
            if hle.card_call(o, t, n):
                return
        if impl is None:
            raise Fatal("BIOS %s(%02X) is not implemented (called from %08x)" % (t, n, o.st.r[31]))
        impl(o)
    oracle.bios = bios
    oracle.on_vblank = hle.on_vblank
    oracle.on_rcnt_irq = hle.on_rcnt_irq
    oracle.chain_walk = hle.chain_walk


def _add_row(exe, addr, name, param, img, param2=0):
    i = (addr - img.base) // 4
    row = "%s,0x%08x,%s,0x%x,0x%x,%s" % (exe, addr, name, param, param2, signature(img.words[i:i + SIG_WORDS]))
    lines = open(HOOKS_CSV).read().splitlines()
    lines = [l for l in lines if not (l.startswith(exe + ",0x%08x," % addr))]
    lines.append(row)
    head, body = lines[0], sorted(lines[1:])
    open(HOOKS_CSV, "w").write("\n".join([head] + body) + "\n")
    print(row)


if __name__ == "__main__":
    # usage: hooks.py add EXE_PATH ADDR NAME [PARAM [PARAM2]]
    from tools.recomp import funcs
    if sys.argv[1] == "add":
        path, addr, name = sys.argv[2], int(sys.argv[3], 16), sys.argv[4]
        if name not in NAMES:
            sys.exit("unknown hook name %s" % name)
        _add_row(os.path.basename(path), addr, name, int(sys.argv[5], 16) if len(sys.argv) > 5 else 0, funcs.Image(path), int(sys.argv[6], 16) if len(sys.argv) > 6 else 0)
