/* Guest memory map (queue P2-8). Source for the layout: psx-spx "Memory Map" (KUSEG/KSEG0/KSEG1 mirror the first 512 MiB; the
 * scratchpad exists in KUSEG and KSEG0 only; 2 MiB RAM, the first 64 KiB reserved for the BIOS). The decision to abort on every
 * region the project has not proven used is docs/decisions/0002 section 3.1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "runtime assumes a little-endian host (decisions/0002 section 3.1)"
#endif

#define RAM_SIZE 0x200000u
#define BIOS_RESERVED 0x10000u
#define RAM_MIRROR_END 0x800000u
#define SCR_BASE 0x1F800000u
#define SCR_SIZE 0x400u
#define IO_LO 0x1F801000u
#define IO_HI 0x1F804000u

typedef struct { uint32_t lo, hi; const char *why; } LowRange;
typedef struct { uint8_t *ram; uint8_t *scr; int allow_mirrors; LowRange low[8]; unsigned nlow; } Mem;

static Mem *M(Cpu *c) { return (Mem *)c->host; }

void rt_init(Cpu *c) {
    rt_cov_init();
    memset(c, 0, sizeof *c);
    c->yield = RT_YIELD_PERIOD;
    c->cop0[12] = 0x401;   /* SR after BIOS boot: IE=1, IM2=1 (assumption: psx-spx does not state the state at DoExecute) */
    Mem *m = calloc(1, sizeof *m);
    if (m) { m->ram = calloc(1, RAM_SIZE); m->scr = calloc(1, SCR_SIZE); }
    if (!m || !m->ram || !m->scr) rt_fatal(NULL, "out of memory allocating guest RAM");
    c->host = m;
}
void rt_shutdown(Cpu *c) {
    Mem *m = M(c);
    if (m) { free(m->ram); free(m->scr); free(m); }
    c->host = NULL;
}
uint8_t *rt_ram_ptr(Cpu *c) { return M(c)->ram; }
void rt_set_ram_mirrors(Cpu *c, int allow) { M(c)->allow_mirrors = allow; }
void rt_allow_low_ram(Cpu *c, uint32_t lo, uint32_t hi, const char *why) {
    Mem *m = M(c);
    if (lo >= hi || hi > BIOS_RESERVED || m->nlow == 8) rt_fatal(c, "rt_allow_low_ram: bad range [%08x,%08x)", lo, hi);
    m->low[m->nlow++] = (LowRange){lo, hi, why};
}

/* ---- code protection ---- */
static uint8_t *g_codebits;                       /* one bit per RAM word */
typedef struct { uint32_t addr; unsigned n; uint32_t w[4]; } Patch;
static Patch g_patch[64];
static unsigned g_npatch;
void rt_code_reset(void) { free(g_codebits); g_codebits = NULL; g_npatch = 0; }
void rt_code_clear_above(uint32_t addr) {
    if (g_codebits) for (uint32_t a = addr & ~3u; a < 0x80200000u; a += 4) { uint32_t i = (a & (RAM_SIZE - 1)) >> 2; g_codebits[i >> 3] &= (uint8_t)~(1u << (i & 7)); }
    unsigned k = 0;
    for (unsigned i = 0; i < g_npatch; i++) if (g_patch[i].addr < (addr & 0x1FFFFFFFu)) g_patch[k++] = g_patch[i];
    g_npatch = k;
}
void rt_code_range(uint32_t lo, uint32_t hi) {
    if (!g_codebits) g_codebits = calloc(RAM_SIZE / 4 / 8, 1);
    for (uint32_t a = lo & ~3u; a < hi; a += 4) { uint32_t i = (a & (RAM_SIZE - 1)) >> 2; g_codebits[i >> 3] |= (uint8_t)(1u << (i & 7)); }
}
void rt_patch_allow(uint32_t addr, unsigned n, const uint32_t *words) {
    if (g_npatch == 64 || n == 0 || n > 4) rt_fatal(NULL, "rt_patch_allow: bad patch at %08x", addr);
    g_patch[g_npatch].addr = addr & 0x1FFFFFFFu; g_patch[g_npatch].n = n;
    for (unsigned k = 0; k < n; k++) g_patch[g_npatch].w[k] = words[k];
    g_npatch++;
}
static int (*g_store_hook)(Cpu *, uint32_t);
void rt_set_code_store_hook(int (*fn)(Cpu *, uint32_t)) { g_store_hook = fn; }
void rt_code_clear_range(uint32_t lo, uint32_t hi) {
    if (!g_codebits) return;
    for (uint32_t a = lo & ~3u; a < hi; a += 4) { uint32_t i = (a & (RAM_SIZE - 1)) >> 2; g_codebits[i >> 3] &= (uint8_t)~(1u << (i & 7)); }
}
static void code_guard(Cpu *c, uint32_t a, unsigned size, uint32_t v) {
    uint32_t phys = a & 0x1FFFFFFFu, i = (phys & (RAM_SIZE - 1)) >> 2;
    if (!(g_codebits[i >> 3] & (1u << (i & 7)))) return;
    if (size == 4) for (unsigned k = 0; k < g_npatch; k++) if (g_patch[k].addr == phys) for (unsigned j = 0; j < g_patch[k].n; j++) if (g_patch[k].w[j] == v) return;
    if (g_store_hook && g_store_hook(c, a)) return;                       /* an overlay that was freed and reused: it is unregistered and the store goes ahead */
    rt_fatal(c, "store of %08x (%u bytes) into recompiled code at %08x: self-modifying code that is not a registered patch", v, size, a);
}

/* ---- I/O devices ---- */
#define MAX_DEV 32
static RtIoDev g_dev[MAX_DEV];
static unsigned g_ndev;

void rt_io_reset(void) { g_ndev = 0; }
void rt_io_register(const RtIoDev *d) {
    if (!d->name || !d->rd || !d->wr || d->lo >= d->hi || d->lo < IO_LO || d->hi > IO_HI)
        rt_fatal(NULL, "rt_io_register: bad device %s [%08x,%08x)", d->name ? d->name : "?", d->lo, d->hi);
    if (g_ndev == MAX_DEV) rt_fatal(NULL, "rt_io_register: device table full");
    for (unsigned i = 0; i < g_ndev; i++)
        if (d->lo < g_dev[i].hi && g_dev[i].lo < d->hi)
            rt_fatal(NULL, "rt_io_register: %s overlaps %s", d->name, g_dev[i].name);
    g_dev[g_ndev++] = *d;
}
static const RtIoDev *find_dev(uint32_t phys, unsigned size) {
    for (unsigned i = 0; i < g_ndev; i++)
        if (phys >= g_dev[i].lo && phys + size <= g_dev[i].hi) return &g_dev[i];
    return NULL;
}

/* ---- address decode ---- */
/* Returns a host pointer for RAM/scratchpad; NULL (with *phys_io set) for I/O; aborts for everything else. */
static uint8_t *decode(Cpu *c, uint32_t a, unsigned size, const char *op, uint32_t *phys_io) {
    Mem *m = M(c);
    if (a & (size - 1))
        rt_fatal(c, "misaligned %s of %u bytes at %08x (hardware would raise AdEL/AdES)", op, size, a);
    uint32_t seg = a >> 29, phys;
    if (seg == 4 || seg == 5 || a < 0x20000000u) phys = a & 0x1FFFFFFFu;   /* KSEG0, KSEG1, KUSEG low 512 MiB (mirrors) */
    else if (seg < 4) rt_fatal(c, "%s at %08x: KUSEG above 512 MiB (hardware raises an exception)", op, a);
    else rt_fatal(c, "%s at %08x: KSEG2 (cache-control ports) is not proven used", op, a);

    if (phys < RAM_SIZE) {
        if (phys < BIOS_RESERVED) {
            int ok = 0;
            for (unsigned i = 0; i < m->nlow; i++) if (phys >= m->low[i].lo && phys + size <= m->low[i].hi) ok = 1;
            if (!ok) rt_fatal(c, "%s at %08x: first 64 KiB of RAM belongs to the BIOS, which is not present", op, a);
        }
        return m->ram + phys;
    }
    if (phys < RAM_MIRROR_END) {
        if (!m->allow_mirrors) rt_fatal(c, "%s at %08x: RAM mirror above 2 MiB (use is unproven; rt_set_ram_mirrors to allow)", op, a);
        return m->ram + (phys & (RAM_SIZE - 1));
    }
    if (phys >= SCR_BASE && phys < SCR_BASE + SCR_SIZE) {
        if (seg == 5) rt_fatal(c, "%s at %08x: scratchpad is not mapped in KSEG1 (psx-spx)", op, a);
        if (phys + size > SCR_BASE + SCR_SIZE) rt_fatal(c, "%s at %08x crosses the end of the scratchpad", op, a);
        return m->scr + (phys - SCR_BASE);
    }
    if (phys >= IO_LO && phys < IO_HI) { *phys_io = phys; return NULL; }
    rt_fatal(c, "%s at %08x (physical %08x): unmapped or unproven region (expansion 1/3, BIOS ROM)", op, a, phys);
}

static uint32_t dev_read(Cpu *c, uint32_t phys, unsigned size) {
    const RtIoDev *d = find_dev(phys, size);
    if (!d) rt_fatal(c, "read of %u bytes from unproven I/O register %08x", size, phys);
    return d->rd(d->ctx, c, phys, size);
}
static void dev_write(Cpu *c, uint32_t phys, unsigned size, uint32_t v) {
    const RtIoDev *d = find_dev(phys, size);
    if (!d) rt_fatal(c, "write of %08x (%u bytes) to unproven I/O register %08x", v, size, phys);
    d->wr(d->ctx, c, phys, size, v);
}

uint32_t rt_lw(Cpu *c, uint32_t a) {
    uint32_t io, v; uint8_t *p = decode(c, a, 4, "load", &io);
    if (!p) return dev_read(c, io, 4);
    memcpy(&v, p, 4); return v;
}
uint32_t rt_lhu(Cpu *c, uint32_t a) {
    uint32_t io; uint16_t v; uint8_t *p = decode(c, a, 2, "load", &io);
    if (!p) return dev_read(c, io, 2) & 0xFFFFu;
    memcpy(&v, p, 2); return v;
}
uint32_t rt_lh(Cpu *c, uint32_t a) { uint32_t v = rt_lhu(c, a); return (v & 0x8000u) ? v | 0xFFFF0000u : v; }
uint32_t rt_lbu(Cpu *c, uint32_t a) {
    uint32_t io; uint8_t *p = decode(c, a, 1, "load", &io);
    return p ? *p : (dev_read(c, io, 1) & 0xFFu);
}
uint32_t rt_lb(Cpu *c, uint32_t a) { uint32_t v = rt_lbu(c, a); return (v & 0x80u) ? v | 0xFFFFFF00u : v; }
void rt_sw(Cpu *c, uint32_t a, uint32_t v) {
    uint32_t io; uint8_t *p = decode(c, a, 4, "store", &io);
    if (!p) dev_write(c, io, 4, v); else { if (g_codebits) code_guard(c, a, 4, v); memcpy(p, &v, 4); }
    rt_trace_store(c, a, 4, v);
}
void rt_sh(Cpu *c, uint32_t a, uint32_t v) {
    uint32_t io; uint16_t h = (uint16_t)v; uint8_t *p = decode(c, a, 2, "store", &io);
    if (!p) dev_write(c, io, 2, v & 0xFFFFu); else { if (g_codebits) code_guard(c, a, 2, v & 0xFFFFu); memcpy(p, &h, 2); }
    rt_trace_store(c, a, 2, v & 0xFFFFu);
}
void rt_sb(Cpu *c, uint32_t a, uint32_t v) {
    uint32_t io; uint8_t *p = decode(c, a, 1, "store", &io);
    if (!p) dev_write(c, io, 1, v & 0xFFu); else { if (g_codebits) code_guard(c, a, 1, v & 0xFFu); *p = (uint8_t)v; }
    rt_trace_store(c, a, 1, v & 0xFFu);
}

/* host-side block access: byte-wise through the same decoder (RAM/scratchpad only, no device side effects) */
void rt_mem_write(Cpu *c, uint32_t addr, const void *src, size_t n) {
    const uint8_t *s = src; uint32_t io;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = decode(c, addr + (uint32_t)i, 1, "host write", &io);
        if (!p) rt_fatal(c, "host write into I/O space at %08x", addr + (uint32_t)i);
        *p = s[i];
    }
}
void rt_mem_read(Cpu *c, uint32_t addr, void *dst, size_t n) {
    uint8_t *d = dst; uint32_t io;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = decode(c, addr + (uint32_t)i, 1, "host read", &io);
        if (!p) rt_fatal(c, "host read from I/O space at %08x", addr + (uint32_t)i);
        d[i] = *p;
    }
}

void rt_exe_load_mem(Cpu *c, const uint8_t *img, size_t n, const char *what, RtExeInfo *out) {
    if (n < 0x800 || memcmp(img, "PS-X EXE", 8) != 0) rt_fatal(c, "%s: not a PS-X EXE (bad magic or short header)", what);
    const uint8_t *h = img;
    #define W(o) ((uint32_t)h[o] | (uint32_t)h[o + 1] << 8 | (uint32_t)h[o + 2] << 16 | (uint32_t)h[o + 3] << 24)
    RtExeInfo e = {W(0x10), W(0x14), W(0x18), W(0x1C), W(0x30), W(0x34)};
    #undef W
    if ((e.addr & 3) || (e.size & 3) || e.size == 0 || e.size > RAM_SIZE) rt_fatal(c, "%s: bad load address %08x / size %08x", what, e.addr, e.size);
    if (n - 0x800 < e.size) rt_fatal(c, "%s: file shorter than header size %08x", what, e.size);
    rt_mem_write(c, e.addr, img + 0x800, e.size);
    *out = e;
}

void rt_exe_load(Cpu *c, const char *path, RtExeInfo *out) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal(c, "cannot open EXE %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)(n > 0 ? n : 1));
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) rt_fatal(c, "cannot read EXE %s", path);
    fclose(f);
    rt_exe_load_mem(c, buf, (size_t)n, path, out);
    free(buf);
}

void rt_load_raw(Cpu *c, const char *path, uint32_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal(c, "cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) rt_fatal(c, "cannot read %s", path);
    fclose(f);
    rt_mem_write(c, addr, buf, (size_t)n);
    free(buf);
}

/* ---- hardware register model (runtime/io.c) over the whole I/O window ---- */
#include "io.h"
static IoCtx *g_hw;
static uint32_t hw_rd(void *ctx, Cpu *c, uint32_t phys, unsigned size) {
    int ok;
    uint32_t v = io_read((IoCtx *)ctx, phys, size, &ok);
    { static int sl = -1; if (sl < 0) sl = getenv("PS1_SIOLOG") != NULL; if (sl && phys >= 0x1F801040 && phys < 0x1F801050) fprintf(stderr, "[sio] pc %08x read %08x (%u) = %08x\n", c->pc, phys, size, v); }
    if (!ok) rt_fatal(c, "%s", io_last_error((IoCtx *)ctx));
    return v;
}
static void hw_wr(void *ctx, Cpu *c, uint32_t phys, unsigned size, uint32_t v) {
    int ok;
    { static int sl = -1; if (sl < 0) sl = getenv("PS1_SIOLOG") != NULL; if (sl && phys >= 0x1F801040 && phys < 0x1F801050) fprintf(stderr, "[sio] pc %08x write %08x (%u) <- %08x\n", c->pc, phys, size, v); }
    static int kdbg = -1; if (kdbg < 0) kdbg = getenv("PS1_KONPC") != NULL;       /* debug aid: print the return address and a0-a3, s0-s3 of the code that writes KON (1F801D88/8A) */
    if (kdbg && (phys == 0x1F801D88 || phys == 0x1F801D8A)) fprintf(stderr, "[konpc] write %04x to %08x ra=%08x a0-a3 %08x %08x %08x %08x s0-s3 %08x %08x %08x %08x\n", v, phys, c->r[31], c->r[4], c->r[5], c->r[6], c->r[7], c->r[16], c->r[17], c->r[18], c->r[19]);
    io_write((IoCtx *)ctx, phys, size, v, &ok);
    if (!ok) rt_fatal(c, "%s", io_last_error((IoCtx *)ctx));
    if (io_dma_pending((IoCtx *)ctx)) rt_dma_kick(c);               /* a completed transfer interrupts after a short time, not at the next frame (finding 0011) */
}
static char g_disc_path[600];
void rt_io_set_disc(const char *p) { snprintf(g_disc_path, sizeof g_disc_path, "%s", p); }
void rt_io_attach_hw(Cpu *c) {
    g_hw = io_new(M(c)->ram);
    if (g_disc_path[0] && !io_cd_open(g_hw, g_disc_path)) rt_fatal(c, "cannot open the disc image %s for the CD controller", g_disc_path);
    RtIoDev d = {"hw", IO_LO, IO_HI, hw_rd, hw_wr, g_hw};
    rt_io_register(&d);
}
struct IoCtx *rt_io_ctx(Cpu *c) { (void)c; return g_hw; }
