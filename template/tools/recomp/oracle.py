"""Reference whole-program MIPS-I interpreter used ONLY as a test oracle for the recompiled C (queue P2-2; decisions/0002 section 4).

It is not shipped and not a game runner: it exists to produce a trace that the recompiled program must reproduce. Instruction semantics are
`spec.py` (written independently of the emitter and tested with known-answer cases); the memory map and its fail-loud policy are written
here a second time, in Python, independently of runtime/mem.c, so that a disagreement between the two shows up in the traces.

Trace = ordered list of events:
  ("st", pc, addr, size, value)       every guest store; swl/swr are logged as the merged aligned word, like the C emitter does (lw + sw)
  ("hle", name, a0, a1, a2, a3, sp, ra)  every call of a hooked guest address (hooks replace guest code; see tools/recomp/hooks.py)
Both sides record the same events; the first difference is reported with the guest pc.
"""
import ctypes
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.recomp import spec  # noqa: E402
from tools.recomp.isa import decode  # noqa: E402

RAM_SIZE, BIOS_RESERVED, MIRROR_END = 0x200000, 0x10000, 0x800000
SCR_BASE, SCR_SIZE, IO_LO, IO_HI = 0x1F800000, 0x400, 0x1F801000, 0x1F804000
CALL_SENTINEL = 0xFFFFFFE0   # return address of a guest function called from a hook (events, callbacks)
SENTINEL = 0xFFFFFFF0   # return address given to the program: reaching it means "the executable returned"


_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # -I does not add the script directory
import hostenv  # noqa: E402


def load_io_lib():
    """Builds (if needed) and loads runtime/io.c as a shared library: the register model is written once, in C, and used by both executors."""
    out = os.path.join(_ROOT, "build", "libps1io" + hostenv.shlib_ext())
    src = [os.path.join(_ROOT, "runtime", "io.c"), os.path.join(_ROOT, "runtime", "io.h"), os.path.join(_ROOT, "runtime", "gte.c"), os.path.join(_ROOT, "runtime", "gte.h"), os.path.join(_ROOT, "runtime", "card.c"), os.path.join(_ROOT, "runtime", "card.h")]
    if not os.path.exists(out) or os.path.getmtime(out) < max(os.path.getmtime(p) for p in src):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        subprocess.run([hostenv.CC, "-std=gnu11", "-O1", "-shared", "-fPIC", "-I", os.path.join(_ROOT, "runtime"), src[0], src[2], src[4], os.path.join(_ROOT, "runtime", "cov_stub.c"), "-o", out], check=True)
    lib = ctypes.CDLL(out)
    lib.io_new.restype = ctypes.c_void_p
    lib.io_new.argtypes = [ctypes.c_void_p]
    lib.io_read.restype = ctypes.c_uint32
    lib.io_read.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
    lib.io_write.restype = None
    lib.io_write.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint, ctypes.c_uint32, ctypes.POINTER(ctypes.c_int)]
    lib.io_last_error.restype = ctypes.c_char_p
    lib.io_last_error.argtypes = [ctypes.c_void_p]
    for nm in ("mfc2", "cfc2"):
        f = getattr(lib, "gte_core_" + nm)
        f.restype = ctypes.c_uint32
        f.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    for nm in ("mtc2", "ctc2"):
        getattr(lib, "gte_core_" + nm).argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint32]
    lib.gte_core_cmd.restype = ctypes.c_int
    lib.gte_core_cmd.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
    lib.card_new.restype = ctypes.c_void_p
    lib.card_new.argtypes = [ctypes.c_char_p]
    lib.card_save.argtypes = [ctypes.c_void_p]
    lib.card_call.restype = ctypes.c_int
    lib.card_call.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_int)]
    lib.card_image.restype = ctypes.c_void_p
    lib.card_image.argtypes = [ctypes.c_void_p]
    lib.io_present_rgb.restype = ctypes.c_int
    lib.io_present_rgb.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    lib.io_vblank_tick.argtypes = [ctypes.c_void_p]
    lib.io_cd_open.restype = ctypes.c_int
    lib.io_cd_open.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.io_sio_irq_next.restype = ctypes.c_int
    lib.io_sio_irq_next.argtypes = [ctypes.c_void_p]
    lib.io_sio_set_pad.restype = None
    lib.io_sio_set_pad.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.io_dma_irq_next.restype = ctypes.c_int
    lib.io_dma_irq_next.argtypes = [ctypes.c_void_p]
    lib.io_cd_step.restype = ctypes.c_int
    lib.io_cd_step.argtypes = [ctypes.c_void_p]
    lib.io_imask.restype = ctypes.c_uint32
    lib.io_imask.argtypes = [ctypes.c_void_p]
    lib.io_irq_ack.restype = None
    lib.io_irq_ack.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.io_timer_irq_next.restype = ctypes.c_int
    lib.io_timer_irq_next.argtypes = [ctypes.c_void_p]
    lib.io_irq_pending.restype = ctypes.c_int
    lib.io_irq_pending.argtypes = [ctypes.c_void_p]
    lib.io_state_hash.restype = ctypes.c_uint64
    lib.io_state_hash.argtypes = [ctypes.c_void_p]
    lib.io_gpu_counters.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)]
    lib.io_dump_ppm.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    return lib


class _GteRegs:
    """COP2 data (`kind` 'd') or control ('c') registers of spec.State, backed by the shared C register file (runtime/gte.c)."""

    def __init__(self, lib, d, c, kind):
        self.lib, self.d, self.c, self.kind = lib, d, c, kind

    def __getitem__(self, n):
        f = self.lib.gte_core_mfc2 if self.kind == "d" else self.lib.gte_core_cfc2
        return f(ctypes.addressof(self.d), ctypes.addressof(self.c), n)

    def __setitem__(self, n, v):
        f = self.lib.gte_core_mtc2 if self.kind == "d" else self.lib.gte_core_ctc2
        f(ctypes.addressof(self.d), ctypes.addressof(self.c), n, v & 0xFFFFFFFF)


class _GteCmds:
    def __init__(self, lib, d, c):
        self.lib, self.d, self.c = lib, d, c

    def append(self, imm25):
        if not self.lib.gte_core_cmd(ctypes.addressof(self.d), ctypes.addressof(self.c), imm25):
            raise Fatal("GTE command %07x is not implemented (not in the psx-spx command list)" % imm25)


class Fatal(Exception):
    pass


class StopRun(Exception):
    pass


class IrqReturn(Exception):
    pass


YIELD_PERIOD = 40000     # must equal RT_YIELD_PERIOD in runtime/rt.h (calibrated against a reference emulator in the ports)
YIELD_OPS = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bc2f", "bc2t", "j"}


class Memory:
    def __init__(self):
        self.ram = bytearray(RAM_SIZE)
        self.scr = bytearray(SCR_SIZE)
        self.allow_mirrors = False
        self.low_ranges = []       # physical [lo, hi) of the first 64 KiB proven used by the program (finding 0019)
        self.devices = []          # (lo, hi, name, rd, wr)
        self.trace = None
        self.quiet = False
        self.cur_pc = 0
        self.on_store = None       # callable(addr, size, value) for every guest store (self-modifying-code detection)

    def _decode(self, a, size, op):
        if a & (size - 1):
            raise Fatal("misaligned %s of %d bytes at %08x" % (op, size, a))
        seg = a >> 29
        if seg in (4, 5) or a < 0x20000000:
            phys = a & 0x1FFFFFFF
        elif seg < 4:
            raise Fatal("%s at %08x: KUSEG above 512 MiB" % (op, a))
        else:
            raise Fatal("%s at %08x: KSEG2" % (op, a))
        if phys < RAM_SIZE:
            if phys < BIOS_RESERVED and not any(lo <= phys and phys + size <= hi for lo, hi in self.low_ranges):
                raise Fatal("%s at %08x: first 64 KiB of RAM (BIOS)" % (op, a))
            return self.ram, phys
        if phys < MIRROR_END:
            if not self.allow_mirrors:
                raise Fatal("%s at %08x: RAM mirror above 2 MiB" % (op, a))
            return self.ram, phys & (RAM_SIZE - 1)
        if SCR_BASE <= phys < SCR_BASE + SCR_SIZE:
            if seg == 5:
                raise Fatal("%s at %08x: scratchpad not mapped in KSEG1" % (op, a))
            if phys + size > SCR_BASE + SCR_SIZE:
                raise Fatal("%s at %08x crosses the end of the scratchpad" % (op, a))
            return self.scr, phys - SCR_BASE
        if IO_LO <= phys < IO_HI:
            return None, phys
        raise Fatal("%s at %08x: unmapped or unproven region" % (op, a))

    def _dev(self, phys, size):
        for lo, hi, name, rd, wr in self.devices:
            if lo <= phys and phys + size <= hi:
                return rd, wr
        return None

    def load(self, a, size):
        buf, o = self._decode(a, size, "load")
        if buf is None:
            d = self._dev(o, size)
            if d is None:
                raise Fatal("read of %d bytes from unproven I/O register %08x" % (size, o))
            return d[0](o, size) & ((1 << (8 * size)) - 1)
        return int.from_bytes(buf[o:o + size], "little")

    def store(self, a, size, v):
        v &= (1 << (8 * size)) - 1
        buf, o = self._decode(a, size, "store")
        if buf is None:
            d = self._dev(o, size)
            if d is None:
                raise Fatal("write of %08x (%d bytes) to unproven I/O register %08x" % (v, size, o))
            d[1](o, size, v)
        else:
            buf[o:o + size] = v.to_bytes(size, "little")
        if self.on_store is not None:
            self.on_store(a, size, v)
        if self.trace is not None and not self.quiet:
            self.trace.append(("st", self.cur_pc, a, size, v))

    # byte view used by spec.execute for lwl/lwr/swl/swr (index = address - spec.MEM_BASE)
    def __getitem__(self, i):
        return self.load(i + spec.MEM_BASE, 1)

    def __setitem__(self, i, v):
        self.quiet = True
        try:
            self.store(i + spec.MEM_BASE, 1, v)
        finally:
            self.quiet = False


class Oracle:
    def __init__(self):
        self.mem = Memory()
        self.st = spec.State()
        self.st.mem = self.mem
        self.trace = []
        self.mem.trace = self.trace
        self.hooks = {}           # guest address (physical form) -> (name, python callable(oracle))
        self.custom_exit = 0      # B(19h) SetCustomExitFromException
        self.in_irq = False
        self.on_vblank = None
        self.on_rcnt_irq = None
        self.chain_walk = None
        self.io = None
        self.ticks = 0
        self.max_ticks = None
        self.yield_count = YIELD_PERIOD
        self.bios = None          # callable(oracle, table, number) for jumps to the BIOS entry points 0xA0 / 0xB0 / 0xC0 (t1 = function number)
        self.icache = {}
        self.smc = []              # (pc, addr, value) of every store into a registered code word
        self.code_words = set()    # addresses of instruction words that recompiled code covers (set by the caller)
        self.mem.on_store = self._watch_store
        self.steps = 0
        spec.load_mem = lambda st, addr, size, pc: self.mem.load(addr, size)
        spec.store_mem = lambda st, addr, size, val, pc: self.mem.store(addr, size, val)
        spec._byte_addr_ok = lambda addr, pc: None

    def _watch_store(self, addr, size, v):
        w = addr & ~3
        for seg in (0x00000000, 0x80000000, 0xA0000000):          # a store to code invalidates the decoded instruction in every segment alias
            self.icache.pop((w & 0x1FFFFFFF) | seg, None)
        if self.code_words and ((w & 0x1FFFFFFF) | 0x80000000) in self.code_words:
            self.smc.append((self.mem.cur_pc, addr, size, v))

    # ---- loading ----
    def load_exe(self, path):
        d = open(path, "rb").read()
        if d[:8] != b"PS-X EXE":
            raise Fatal("%s: not a PS-X EXE" % path)
        pc, gp, addr, size, _, _, _, _, sp_base, sp_off = struct.unpack("<10I", d[0x10:0x38])
        if addr & 3 or size & 3 or size == 0 or size > RAM_SIZE:
            raise Fatal("bad load address/size")
        for k in range(size):
            self.mem.quiet = True
            buf, o = self.mem._decode(addr + k, 1, "host write")
            buf[o] = d[0x800 + k]
        self.mem.quiet = False
        r = self.st.r
        r[28] = gp
        if sp_base:
            r[29] = r[30] = (sp_base + sp_off) & 0xFFFFFFFF
        r[31] = SENTINEL
        self.st.pc = pc
        self.st.cop0[12] = 0x401          # SR after BIOS boot (assumption, same as runtime/mem.c)
        return pc

    def attach_hw(self, disc_path=None):
        """Registers the shared C register model (runtime/io.c) over the I/O window."""
        self.io = load_io_lib()
        d, c = (ctypes.c_uint32 * 32)(), (ctypes.c_uint32 * 32)()
        self._gte = (d, c)
        self.st.cop2d = _GteRegs(self.io, d, c, "d")
        self.st.cop2c = _GteRegs(self.io, d, c, "c")
        self.st.cop2cmds = _GteCmds(self.io, d, c)
        self._ram_c = (ctypes.c_char * RAM_SIZE).from_buffer(self.mem.ram)
        self.io_ctx = self.io.io_new(ctypes.addressof(self._ram_c))
        if disc_path is not None and not self.io.io_cd_open(self.io_ctx, disc_path.encode()):
            raise Fatal("cannot open the disc image")

        def rd(phys, size):
            ok = ctypes.c_int()
            v = self.io.io_read(self.io_ctx, phys, size, ctypes.byref(ok))
            if not ok.value:
                raise Fatal(self.io.io_last_error(self.io_ctx).decode())
            return v

        def wr(phys, size, v):
            ok = ctypes.c_int()
            self.io.io_write(self.io_ctx, phys, size, v, ctypes.byref(ok))
            if not ok.value:
                raise Fatal(self.io.io_last_error(self.io_ctx).decode())
        self.mem.devices.append((IO_LO, IO_HI, "hw", rd, wr))

    def hw_write(self, phys, size, v):
        ok = ctypes.c_int()
        self.io.io_write(self.io_ctx, phys, size, v, ctypes.byref(ok))
        if not ok.value:
            raise Fatal(self.io.io_last_error(self.io_ctx).decode())

    def hw_read(self, phys, size):
        ok = ctypes.c_int()
        v = self.io.io_read(self.io_ctx, phys, size, ctypes.byref(ok))
        if not ok.value:
            raise Fatal(self.io.io_last_error(self.io_ctx).decode())
        return v

    def present(self):
        """Display area as (width, height, rgb bytes, enabled)."""
        buf = (ctypes.c_uint8 * (1024 * 512 * 3))()
        w, h = ctypes.c_int(), ctypes.c_int()
        on = self.io.io_present_rgb(self.io_ctx, ctypes.addressof(buf), 1024, 512, ctypes.byref(w), ctypes.byref(h))
        return w.value, h.value, bytes(buf[:w.value * h.value * 3]), bool(on)

    def hw_hash(self):
        return self.io.io_state_hash(self.io_ctx)

    def irq_deliver(self):
        st = self.st
        if self.in_irq or not self.custom_exit or not (st.cop0[12] & 1) or not self.io.io_irq_pending(self.io_ctx):
            return
        r = st.r
        self.trace.append(("hle", "IRQ", r[4], r[5], r[6], r[7], r[29], r[31]))
        spec.flush(st)      # a load in flight completes before the interrupt is taken (the instruction after the handler sees the new value)
        saved = (list(st.r), st.hi, st.lo, st.cop0[12], st.pending, st.pc)
        jb = [self.mem.load(self.custom_exit + 4 * k, 4) for k in range(12)]    # psx-spx B(19h): SaveState-format structure
        entry = jb[0]
        st.r[29], st.r[30] = jb[1], jb[2]
        st.r[16:24] = jb[3:11]
        st.r[28], st.r[2] = jb[11], 1
        st.cop0[12] &= ~1
        self.in_irq = True
        if self.chain_walk:
            self.chain_walk(self)
            st.r[2] = 1
        st.pc = entry
        try:
            self.run(10_000_000, nested=1)
        except IrqReturn:
            pass
        self.in_irq = False
        st.r[:] = saved[0]
        st.hi, st.lo, st.cop0[12], st.pending, st.pc = saved[1], saved[2], saved[3], saved[4], saved[5]

    def call_guest(self, addr, a0=0, a1=0, a2=0, a3=0):
        """Calls a guest function from a hook (HLE'd BIOS event delivery): registers are saved and restored, the callee returns to CALL_SENTINEL."""
        st = self.st
        spec.flush(st)
        saved = (list(st.r), st.hi, st.lo, st.pending, st.pc)
        st.r[4:8] = [a0, a1, a2, a3]
        st.r[31] = CALL_SENTINEL
        st.pc = addr
        res = self.run(10_000_000, nested=2)
        if res != "call-returned":
            raise Fatal("guest call to %08x ended with %s" % (addr, res))
        v0 = st.r[2]
        st.r[:] = saved[0]
        st.hi, st.lo, st.pending, st.pc = saved[1], saved[2], saved[3], saved[4]
        return v0

    def tick(self):
        self.yield_count = YIELD_PERIOD
        if self.io is None:        # no hardware model attached (offline uses such as the overlay relocation): time does not pass
            return
        self.ticks += 1
        if self.max_ticks is not None and self.ticks > self.max_ticks:
            raise Fatal("tick limit reached")
        self.io.io_vblank_tick(self.io_ctx)
        if self.on_vblank:
            self.on_vblank(self)
        self.irq_deliver()
        for _ in range(8):                         # root-counter interrupts of the frame (finding 0040)
            t = self.io.io_timer_irq_next(self.io_ctx)
            if not t:
                break
            if self.on_rcnt_irq:
                self.on_rcnt_irq(self, t - 1)
            self.irq_deliver()
        for _ in range(32):                        # SIO0 ACK interrupts: one handler run per transferred byte (finding 0006)
            if not self.io.io_sio_irq_next(self.io_ctx):
                break
            self.irq_deliver()
        for _ in range(64):                        # DMA completion interrupts, one handler run each (finding 0003)
            if not self.io.io_dma_irq_next(self.io_ctx):
                break
            self.irq_deliver()
        for _ in range(3):                         # more CD interrupts within the frame (sector streaming)
            if not self.io.io_cd_step(self.io_ctx):
                break
            self.irq_deliver()

    def hook(self, addr, name, fn):
        self.hooks[addr & 0x1FFFFFFF] = (name, fn)

    # ---- running ----
    def fetch(self, pc):
        x = self.icache.get(pc)
        if x is None:
            buf, o = self.mem._decode(pc, 4, "fetch")
            if buf is None:
                raise Fatal("fetch from I/O space at %08x" % pc)
            x = self.icache[pc] = decode(int.from_bytes(buf[o:o + 4], "little"), pc)
        return x

    def _exec(self, ins, in_slot=False):
        self.mem.cur_pc = ins.pc
        if ins.name in ("swl", "swr"):
            st = self.st
            a = (st.r[ins.rs] + ins.simm) & 0xFFFFFFFF
            self.mem.quiet = True
            try:
                res = spec.execute(st, ins, in_slot)
            finally:
                self.mem.quiet = False
            wa = a & ~3
            self.mem.trace.append(("st", ins.pc, wa, 4, self.mem.load(wa, 4)))
            return res
        try:
            return spec.execute(self.st, ins, in_slot)
        except spec.Exc as e:
            raise Fatal("guest exception %d at %08x arg %x" % (e.code, e.pc, e.arg))

    def run(self, max_steps=50_000_000, nested=0):
        st = self.st
        pc = st.pc
        steps0 = self.steps
        while True:
            self.steps += 1
            if self.steps - steps0 > max_steps:
                raise Fatal("step limit reached at %08x" % pc)
            if pc == SENTINEL:
                st.pc = pc
                return "returned"
            if pc == CALL_SENTINEL and nested == 2:
                st.pc = pc
                return "call-returned"
            if pc in (0xA0, 0xB0, 0xC0) and self.bios is not None:
                spec.flush(st)
                r = st.r
                t = "ABC"[(pc - 0xA0) >> 4]
                self.trace.append(("hle", "BIOS_%s%02X" % (t, r[9]), r[4], r[5], r[6], r[7], r[29], r[31]))
                st.pc = pc
                try:
                    self.bios(self, t, r[9])
                except StopRun:
                    return "stopped at BIOS_%s%02X" % (t, r[9])
                pc = r[31]
                continue
            h = self.hooks.get(pc & 0x1FFFFFFF)
            if h is not None:
                spec.flush(st)
                r = st.r
                self.trace.append(("hle", h[0], r[4], r[5], r[6], r[7], r[29], r[31]))
                st.pc = pc
                try:
                    nxt = h[1](self)
                except StopRun:
                    return "stopped at " + h[0]
                pc = r[31] if nxt is None else nxt
                continue
            ins = self.fetch(pc)
            res = self._exec(ins)
            if res[0] == "branch":
                slot = self.fetch(pc + 4)
                if slot.is_control:
                    raise Fatal("control transfer in a delay slot at %08x" % (pc + 4))
                self._exec(slot, in_slot=True)
                taken_back = res[1] and ins.name in YIELD_OPS and res[2] <= ins.pc
                pc = res[2] if res[1] else pc + 8
                if taken_back:
                    self.yield_count -= 1
                    if self.yield_count <= 0:
                        st.pc = pc
                        self.tick()
            else:
                pc += 4
            st.pc = pc


def diff_traces(a, b):
    """Index of the first differing event (or None) plus the two events."""
    for k in range(min(len(a), len(b))):
        if a[k] != b[k]:
            return k, a[k], b[k]
    if len(a) != len(b):
        return min(len(a), len(b)), (a[len(b)] if len(a) > len(b) else None), (b[len(a)] if len(b) > len(a) else None)
    return None
