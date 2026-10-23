/* BIOS "C library" calls: string/memory functions, setjmp, rand, console output, interrupt-chain bookkeeping. Twin of tools/recomp/biosc.py (same contracts, same INFERRED choices,
 * listed in docs/stubs.md); included by hle.c. Stores go through rt_mem_write (not part of the traced event stream), like the other BIOS hooks. */
static uint32_t g_biosc_rand = 1;            /* INFERRED initial seed */
static uint32_t g_biosc_chain[4];

static void bc_sb(Cpu *c, uint32_t a, uint8_t v) { rt_mem_write(c, a, &v, 1); }
static void bc_sw(Cpu *c, uint32_t a, uint32_t v) { rt_mem_write(c, a, &v, 4); }

static int biosc_cmp(Cpu *c, uint32_t a, uint32_t b, uint32_t limit, int bounded) {
    for (uint32_t i = 0; !bounded || i < limit; i++) {
        uint8_t x = (uint8_t)rt_lbu(c, a + i), y = (uint8_t)rt_lbu(c, b + i);
        if (x != y) return (int)x - (int)y;
        if (x == 0) return 0;
    }
    return 0;
}

/* returns 1 when the call was handled here */
static int biosc_call(Cpu *c, char t, unsigned n) {
    uint32_t *r = c->r;
    if (t == 'A') switch (n) {
    case 0x13: {                                                     /* SetJmp: {ra, sp, fp, s0..s7, gp}, returns 0 */
        uint32_t v[12] = {r[31], r[29], r[30], r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23], r[28]};
        for (int i = 0; i < 12; i++) bc_sw(c, r[4] + 4u * (uint32_t)i, v[i]);
        r[2] = 0; return 1;
    }
    case 0x1B: { uint32_t k = 0; while (rt_lbu(c, r[4] + k)) k++; r[2] = k; return 1; }
    case 0x19: { uint32_t d = r[4], s = r[5], i = 0; for (;; i++) { uint8_t ch = (uint8_t)rt_lbu(c, s + i); bc_sb(c, d + i, ch); if (!ch) break; } r[2] = d; return 1; }
    case 0x15: {
        uint32_t d = r[4], s = r[5], k = 0, i = 0;
        while (rt_lbu(c, d + k)) k++;
        for (;; i++) { uint8_t ch = (uint8_t)rt_lbu(c, s + i); bc_sb(c, d + k + i, ch); if (!ch) break; }
        r[2] = d; return 1;
    }
    case 0x17: r[2] = (uint32_t)biosc_cmp(c, r[4], r[5], 0, 0); return 1;
    case 0x18: r[2] = (uint32_t)biosc_cmp(c, r[4], r[5], r[6], 1); return 1;
    case 0x2A: { uint32_t d = r[4], s = r[5], k = r[6]; for (uint32_t i = 0; i < k; i++) bc_sb(c, d + i, (uint8_t)rt_lbu(c, s + i)); r[2] = d; return 1; }
    case 0x27: { uint32_t s = r[4], d = r[5], k = r[6]; for (uint32_t i = 0; i < k; i++) bc_sb(c, d + i, (uint8_t)rt_lbu(c, s + i)); r[2] = s; return 1; }
    case 0x28: { uint32_t d = r[4], k = r[5]; for (uint32_t i = 0; i < k; i++) bc_sb(c, d + i, 0); r[2] = d; return 1; }
    case 0x2B: { uint32_t d = r[4], k = r[6]; uint8_t v = (uint8_t)r[5]; for (uint32_t i = 0; i < k; i++) bc_sb(c, d + i, v); r[2] = d; return 1; }
    case 0x2E: { uint32_t p = r[4], k = r[6]; uint8_t v = (uint8_t)r[5]; r[2] = 0; for (uint32_t i = 0; i < k; i++) if ((uint8_t)rt_lbu(c, p + i) == v) { r[2] = p + i; break; } return 1; }
    case 0x2F: g_biosc_rand = g_biosc_rand * 0x41C64E6Du + 0x3039u; r[2] = (g_biosc_rand >> 16) & 0x7FFF; return 1;
    case 0x30: g_biosc_rand = r[4]; return 1;
    default: return 0;
    }
    if (t == 'C' && (n == 0x02 || n == 0x03)) {                      /* SysEnqIntRP / SysDeqIntRP bookkeeping */
        uint32_t prio = r[4], ent = r[5];
        if (prio > 3) rt_fatal(c, "%s priority %u", n == 2 ? "SysEnqIntRP" : "SysDeqIntRP", prio);
        if (n == 2) { bc_sw(c, ent, g_biosc_chain[prio]); g_biosc_chain[prio] = ent; r[2] = 0; return 1; }
        uint32_t ret = 0, ptr = g_biosc_chain[prio], prev = 0;
        for (int k = 0; ptr && k < 100; k++) {
            uint32_t nxt = rt_lw(c, ptr);
            if (ptr == ent) { if (prev) bc_sw(c, prev, nxt); else g_biosc_chain[prio] = nxt; ret = prev ? prev : ptr; break; }
            prev = ptr; ptr = nxt;
        }
        r[2] = ret; return 1;
    }
    if ((t == 'B' && (n == 0x3D || n == 0x3F)) || (t == 'A' && (n == 0x3C || n == 0x3E || n == 0x3F))) {      /* console output: B(3Dh) = putchar, B(3Fh) = puts */
        h_stdio(c, (t == 'B' && n == 0x3D) ? 0x3C : (t == 'B' && n == 0x3F) ? 0x3E : n);
        return 1;
    }
    return 0;
}

/* The BIOS exception handler's chain walk (psx-spx "Exception handling"; entry layout as used by the program's own chain entry): for priority 0..3, for every entry of the list
 * {+0 next, +4 handler, +8 test}: call the test function; when it returns non-zero call the handler. Run before the custom exit (finding 0006). Twin: Hle.chain_walk in hooks.py. */
static void biosc_chain_walk(Cpu *c) {
    for (int prio = 0; prio < 4; prio++) {
        uint32_t ptr = g_biosc_chain[prio];
        for (int guard = 0; ptr && guard < 100; guard++) {
            uint32_t f_handler = rt_lw(c, ptr + 4), f_test = rt_lw(c, ptr + 8), v = 1;
            if (f_test) { c->r[31] = 0xFFFFFFE0u; rt_call(c, f_test); v = c->r[2]; }
            if (v && f_handler) { c->r[31] = 0xFFFFFFE0u; rt_call(c, f_handler); }
            ptr = rt_lw(c, ptr);
        }
    }
}
