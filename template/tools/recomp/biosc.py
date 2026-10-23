"""BIOS A-table "C library" calls (string/memory functions, setjmp, rand), executed by the oracle. The C twin is runtime/biosc.inc: both must stay identical
(tests/recomp/test_biosc.py runs the same cases through both). Contracts: psx-spx "BIOS Memory/String functions" for the argument order and the return value where it
states one; where it does not (return value of strcmp, bcopy, bzero; the initial rand seed) the choice is tagged INFERRED below and listed in docs/stubs.md.
All stores are quiet (not part of the traced event stream), like the other BIOS hooks."""
M32 = 0xFFFFFFFF


class LibC:
    NAMES = {("A", 0x13): "SetJmp", ("A", 0x15): "strcat", ("A", 0x17): "strcmp", ("A", 0x18): "strncmp", ("A", 0x19): "strcpy", ("A", 0x1B): "strlen",
             ("A", 0x27): "bcopy", ("A", 0x28): "bzero", ("A", 0x2A): "memcpy", ("A", 0x2B): "memset", ("A", 0x2E): "memchr", ("A", 0x2F): "rand", ("A", 0x30): "srand",
             ("B", 0x3D): "putchar", ("B", 0x3F): "puts", ("C", 0x02): "SysEnqIntRP", ("C", 0x03): "SysDeqIntRP", ("A", 0x3C): "putchar", ("A", 0x3E): "puts", ("A", 0x3F): "printf"}
    rand_seed = 1          # INFERRED initial value (C convention); the real kernel's value is unknown

    @staticmethod
    def _b(o, a):
        return o.mem.load(a & M32, 1)

    @staticmethod
    def _sb(o, a, v):
        o.mem.quiet = True
        o.mem.store(a & M32, 1, v & 0xFF)
        o.mem.quiet = False

    def bios_SetJmp(self, o):                     # psx-spx A(13h): buf = {ra, sp, fp, s0..s7, gp}; returns 0
        r = o.st.r
        vals = [r[31], r[29], r[30]] + [r[16 + i] for i in range(8)] + [r[28]]
        o.mem.quiet = True
        for i, v in enumerate(vals):
            o.mem.store((r[4] + 4 * i) & M32, 4, v)
        o.mem.quiet = False
        r[2] = 0

    def bios_strlen(self, o):
        a = o.st.r[4]
        n = 0
        while self._b(o, a + n):
            n += 1
        o.st.r[2] = n

    def bios_strcpy(self, o):
        r = o.st.r
        d, s, i = r[4], r[5], 0
        while True:
            c = self._b(o, s + i)
            self._sb(o, d + i, c)
            if not c:
                break
            i += 1
        r[2] = d

    def bios_strcat(self, o):
        r = o.st.r
        d, s, n = r[4], r[5], 0
        while self._b(o, d + n):
            n += 1
        i = 0
        while True:
            c = self._b(o, s + i)
            self._sb(o, d + n + i, c)
            if not c:
                break
            i += 1
        r[2] = d

    def _cmp(self, o, a, b, limit):
        i = 0
        while limit is None or i < limit:
            x, y = self._b(o, a + i), self._b(o, b + i)
            if x != y:
                return (x - y) & M32                  # INFERRED: difference of the first differing unsigned bytes
            if x == 0:
                return 0
            i += 1
        return 0

    def bios_strcmp(self, o):
        r = o.st.r
        r[2] = self._cmp(o, r[4], r[5], None)

    def bios_strncmp(self, o):
        r = o.st.r
        r[2] = self._cmp(o, r[4], r[5], r[6])

    def bios_memcpy(self, o):                     # forward byte copy, returns dst
        r = o.st.r
        d, s, n = r[4], r[5], r[6]
        for i in range(n):
            self._sb(o, d + i, self._b(o, s + i))
        r[2] = d

    def bios_bcopy(self, o):                      # bcopy(src, dst, n); INFERRED return value: src (a0), as a published emulator does
        r = o.st.r
        s, d, n = r[4], r[5], r[6]
        for i in range(n):
            self._sb(o, d + i, self._b(o, s + i))
        r[2] = s

    def bios_bzero(self, o):                      # INFERRED return value: dst
        r = o.st.r
        d, n = r[4], r[5]
        for i in range(n):
            self._sb(o, d + i, 0)
        r[2] = d

    def bios_memset(self, o):
        r = o.st.r
        d, c, n = r[4], r[5] & 0xFF, r[6]
        for i in range(n):
            self._sb(o, d + i, c)
        r[2] = d

    def bios_memchr(self, o):
        r = o.st.r
        p, c, n = r[4], r[5] & 0xFF, r[6]
        for i in range(n):
            if self._b(o, p + i) == c:
                r[2] = (p + i) & M32
                return
        r[2] = 0

    def bios_rand(self, o):                       # psx-spx: seed = seed * 41C64E6Dh + 3039h; returns (seed >> 16) & 7FFFh
        LibC.rand_seed = (LibC.rand_seed * 0x41C64E6D + 0x3039) & M32
        o.st.r[2] = (LibC.rand_seed >> 16) & 0x7FFF

    def bios_srand(self, o):
        LibC.rand_seed = o.st.r[4] & M32

    # ---- console output (psx-spx A(3Ch) putchar, A(3Eh) puts, A(3Fh) printf): appended to the captured stdout; twin of h_stdio in runtime/hle.c ----
    def bios_putchar(self, o):
        self.stdout.append(o.st.r[4] & 0xFF)
        o.st.r[2] = o.st.r[4] & 0xFF

    def bios_puts(self, o):
        p = o.st.r[4]
        while self._b(o, p):
            self.stdout.append(self._b(o, p))
            p += 1
        self.stdout.append(10)
        o.st.r[2] = 1

    def bios_printf(self, o):
        r = o.st.r
        p, argi, sp = r[4], [0], r[29]
        start = len(self.stdout)

        def nextarg():
            i = argi[0]
            argi[0] += 1
            return r[5 + i] if i < 3 else o.mem.load((sp + 16 + 4 * (i - 3)) & M32, 4)
        b = lambda a: self._b(o, a)  # noqa: E731
        while b(p):
            ch = b(p)
            if ch != 0x25:
                self.stdout.append(ch)
                p += 1
                continue
            left = zero = alt = False
            width, prec = 0, -1
            while True:
                f = b(p + 1)
                if f == 0x2D:
                    left = True
                elif f == 0x30:
                    zero = True
                elif f == 0x23:
                    alt = True
                elif f in (0x2B, 0x20):
                    pass
                else:
                    break
                p += 1
            if b(p + 1) == 0x2A:
                width = nextarg()
                width = width - (1 << 32) if width & 0x80000000 else width
                p += 1
                if width < 0:
                    left, width = True, -width
            else:
                while 0x30 <= b(p + 1) <= 0x39:
                    width = width * 10 + b(p + 1) - 0x30
                    p += 1
            if b(p + 1) == 0x2E:
                p += 1
                prec = 0
                if b(p + 1) == 0x2A:
                    prec = nextarg()
                    prec = prec - (1 << 32) if prec & 0x80000000 else prec
                    p += 1
                else:
                    while 0x30 <= b(p + 1) <= 0x39:
                        prec = prec * 10 + b(p + 1) - 0x30
                        p += 1
            while b(p + 1) in (0x6C, 0x68):
                p += 1
            p += 1
            cv = chr(b(p))
            if cv == "%":
                s = "%"
            elif cv == "c":
                s = chr(nextarg() & 0xFF)
            elif cv == "s":
                q, s = nextarg(), ""
                while b(q) and len(s) < 560 and (prec < 0 or len(s) < prec):
                    s += chr(b(q))
                    q += 1
            elif cv in "di":
                v = nextarg()
                s = str(v - (1 << 32) if v & 0x80000000 else v)
            elif cv == "u":
                s = str(nextarg())
            elif cv == "x":
                v = nextarg()
                s = ("0x%x" if alt and v else "%x") % v
            elif cv == "X":
                v = nextarg()
                s = ("0X%X" if alt and v else "%X") % v
            elif cv == "p":
                s = "%X" % nextarg()
            else:
                self.stdout.append(0x25)
                self.stdout.append(ord(cv))
                p += 1
                continue
            if alt and cv in "xX" and zero and not left and width > len(s) > 2:
                s = s[:2] + "0" * (width - len(s)) + s[2:]
            pad = max(0, width - len(s))
            fill = "0" if zero and cv not in "sc" else " "
            s = (s + " " * pad) if left else (fill * pad + s)
            self.stdout.extend(s.encode("latin-1"))
            p += 1
        r[2] = (len(self.stdout) - start) & M32

    # ---- kernel interrupt chains (C(02h) SysEnqIntRP, C(03h) SysDeqIntRP): bookkeeping only. The head of each priority's list is held by the HLE layer, the next-pointer is written into the
    # caller's own entry word 0 (as the kernel does). Nothing walks the chains yet: interrupts reach the game through SetCustomExit (oracle.irq_deliver). Twin: runtime/biosc.inc ----
    chain_heads = None

    def bios_SysEnqIntRP(self, o):
        r = o.st.r
        if LibC.chain_heads is None:
            LibC.chain_heads = [0, 0, 0, 0]
        prio, ent = r[4], r[5]
        if prio > 3:
            raise RuntimeError("SysEnqIntRP priority %d" % prio)
        o.mem.quiet = True
        o.mem.store(ent, 4, LibC.chain_heads[prio])
        o.mem.quiet = False
        LibC.chain_heads[prio] = ent
        r[2] = 0

    def bios_SysDeqIntRP(self, o):
        r = o.st.r
        if LibC.chain_heads is None:
            LibC.chain_heads = [0, 0, 0, 0]
        prio, ent = r[4], r[5]
        if prio > 3:
            raise RuntimeError("SysDeqIntRP priority %d" % prio)
        ret, ptr, prev = 0, LibC.chain_heads[prio], 0
        n = 0
        while ptr and n < 100:
            nxt = o.mem.load(ptr, 4)
            if ptr == ent:
                if prev:
                    o.mem.quiet = True
                    o.mem.store(prev, 4, nxt)
                    o.mem.quiet = False
                else:
                    LibC.chain_heads[prio] = nxt
                ret = ptr if not prev else prev
                break
            prev, ptr, n = ptr, nxt, n + 1
        r[2] = ret

    def chain_walk(self, o):                      # twin of biosc_chain_walk in runtime/biosc.inc
        heads = LibC.chain_heads or [0, 0, 0, 0]
        for prio in range(4):
            ptr, guard = heads[prio], 0
            while ptr and guard < 100:
                f_handler, f_test = o.mem.load(ptr + 4, 4), o.mem.load(ptr + 8, 4)
                v = o.call_guest(f_test) if f_test else 1
                if v and f_handler:
                    o.call_guest(f_handler)
                ptr = o.mem.load(ptr, 4)
                guard += 1

