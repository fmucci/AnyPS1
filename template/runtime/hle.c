/* HLE of the launcher's (SLUS_002.36) library calls, queue P2-3. Contracts are PROVISIONAL and mirror tools/recomp/hooks.py exactly;
 * they are listed in docs/stubs.md and examined for fidelity in queue P3-1/P3-2. Every unproven case aborts. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disc.h"
#include "rt.h"

#include "io.h"
#include "card.h"
static Card *g_card;
static uint32_t hwr(Cpu *c, uint32_t a);
static void hww(Cpu *c, uint32_t a, uint32_t v);
static void bios_dispatch(Cpu *c, char t, unsigned n);
static uint32_t g_pad_dest;
static void store_quiet(Cpu *c, uint32_t a, uint32_t v);
static void ev_deliver(Cpu *c, uint32_t cls, uint32_t spec);
static uint32_t g_pend[64][3];                      /* class, spec, frames still to wait (card operations take a few frames in the reference BIOS) */
static unsigned g_npend;
static uint32_t g_custom_exit;      /* B(19h) SetCustomExitFromException: the library's interrupt handler */
static int g_in_irq;
static int g_async_events;       /* >0 while rt_yield delivers events: those callbacks run in the BIOS's exception context, on the exception stack, not on the interrupted thread's stack (finding 0008) */
static int g_chain;
static char g_last_file[40];
typedef struct { char name[40]; void (*code)(void), (*hooks)(void), (*reg)(void); } Module;
static Module g_mod[8];
static unsigned g_nmod;
void rt_hle_set_chain(int on) { g_chain = on; }
void rt_hle_add_module(const char *n, void (*code)(void), void (*hooks)(void), void (*reg)(void)) {
    if (g_nmod == 8) rt_fatal(NULL, "too many modules");
    snprintf(g_mod[g_nmod].name, sizeof g_mod[g_nmod].name, "%s", n); g_mod[g_nmod].code = code; g_mod[g_nmod].hooks = hooks; g_mod[g_nmod].reg = reg; g_nmod++;
}
static jmp_buf g_irq_jb;
static RtDisc *g_disc;
uint8_t *rt_hle_disc_file(const char *iso_path, size_t *n) {
    uint32_t lba, size; char name[32];
    if (!g_disc || !rt_disc_find(g_disc, iso_path, &lba, &size, name)) return NULL;
    uint32_t sectors = (size + 2047) / 2048;
    uint8_t *tmp = malloc((size_t)sectors * 2048 + 1);
    if (!tmp || !rt_disc_read(g_disc, lba + 0, sectors, tmp)) { free(tmp); return NULL; }
    *n = size;
    return tmp;
}
static int g_have_loc;
static uint32_t g_loc;
static char g_out[1 << 16];
static size_t g_outn;
static uint32_t g_exec[3];
static jmp_buf g_stop;
static unsigned g_ticks;

/* debug aid: env PS1_STOPTEXT=text stops (fatal, with the recent-call ring when built with -DRT_CALLTRACE) as soon as the captured console output contains it */
static void stop_text_check(Cpu *c) {
    const char *t = getenv("PS1_STOPTEXT");
    if (t && g_outn >= strlen(t) && g_outn < sizeof g_out) { g_out[g_outn] = 0; if (strstr(g_out, t)) rt_fatal(c, "stop text \"%s\" appeared in the console output", t); }
}
static unsigned bcd(unsigned b) { return (b >> 4) * 10 + (b & 15); }
static unsigned tobcd(unsigned v) { return ((v / 10) << 4) | (v % 10); }

static void h_file_write(Cpu *c) {
    uint32_t fd = c->r[4], src = c->r[5], n = c->r[6];
    if (fd != 1) rt_fatal(c, "FileWrite to fd %u (only stdout is proven used)", fd);
    for (uint32_t k = 0; k < n; k++) {
        uint8_t b = (uint8_t)rt_lbu(c, src + k);
        if (g_outn < sizeof g_out) g_out[g_outn++] = (char)b;
    }
    stop_text_check(c);
    c->r[2] = n;
}
static void h_intr_init(Cpu *c) { rt_trace_hle(c, "IntrInit"); c->r[2] = c->hook_param;   /* address of the library's state block (tools/recomp/hooks.csv) */ }
static void h_cd_init(Cpu *c) { rt_trace_hle(c, "CdInit"); c->r[2] = 1; }
static void h_cd_control_impl(Cpu *c) {
    uint32_t cmd = c->r[4] & 0xFF, param = c->r[5], result = c->r[6];
    if (cmd != 2 && cmd != 0x15 && cmd != 0x09 && cmd != 0x0B && cmd != 0x0C && cmd != 0x0D && cmd != 0x0E && cmd != 0x1B && cmd != 0x06) rt_fatal(c, "CdControl command %#x not implemented", cmd);
    if ((cmd == 2 || cmd == 0x15) && param) {
        unsigned m = rt_lbu(c, param), s = rt_lbu(c, param + 1), f = rt_lbu(c, param + 2);
        g_loc = (bcd(m) * 60 + bcd(s)) * 75 + bcd(f) - 150;
        g_have_loc = 1;
    }
    if (result) { uint8_t st = 0x02; rt_mem_write(c, result, &st, 1); }
    c->r[2] = 1;
}
static void h_cd_control(Cpu *c) { rt_trace_hle(c, "CdControl"); h_cd_control_impl(c); }
static void h_cd_sync(Cpu *c) { rt_trace_hle(c, "CdSync"); c->r[2] = 2; }
static void h_cd_control_b(Cpu *c) { rt_trace_hle(c, "CdControlB"); h_cd_control_impl(c); }
static void h_cd_read(Cpu *c) {
    rt_trace_hle(c, "CdRead");
    uint32_t n = c->r[4], buf = c->r[5], mode = c->r[6];
    if (!g_have_loc || mode != 0x80) rt_fatal(c, "CdRead without Setloc or with mode %#x", mode);
    uint8_t *tmp = malloc((size_t)n * 2048);
    if (!tmp || !rt_disc_read(g_disc, g_loc, n, tmp)) rt_fatal(c, "CdRead: cannot read %u sectors at lba %u", n, g_loc);
    rt_mem_write(c, buf, tmp, (size_t)n * 2048);
    free(tmp);
    c->r[2] = 1;
}
static void h_cd_read_sync(Cpu *c) { rt_trace_hle(c, "CdReadSync"); c->r[2] = 0; }
static void h_cd_search_file(Cpu *c) {
    rt_trace_hle(c, "CdSearchFile");
    uint32_t rec = c->r[4], p = c->r[5];
    char path[256];
    size_t i = 0;
    for (;; i++) {
        if (i + 1 >= sizeof path) rt_fatal(c, "CdSearchFile: path too long");
        path[i] = (char)rt_lbu(c, p + i);
        if (!path[i]) break;
    }
    uint32_t lba, size; char name[32];
    if (!rt_disc_find(g_disc, path, &lba, &size, name)) { c->r[2] = 0; return; }
    { char nm2[32]; snprintf(nm2, sizeof nm2, "%s", name); char *sc = strchr(nm2, ';'); if (sc) *sc = 0; snprintf(g_last_file, sizeof g_last_file, "%s", nm2); }   /* remembered for DoExecute */
    uint32_t n = lba + 150;
    uint8_t blob[24] = {0};
    blob[0] = (uint8_t)tobcd(n / 4500); blob[1] = (uint8_t)tobcd((n / 75) % 60); blob[2] = (uint8_t)tobcd(n % 75); blob[3] = 0;
    for (int k = 0; k < 4; k++) blob[4 + k] = (uint8_t)(size >> (8 * k));
    memcpy(blob + 8, name, strnlen(name, 16));
    rt_mem_write(c, rec, blob, 24);
    c->r[2] = rec;
}
static void store_word(Cpu *c, uint32_t a, uint32_t v) { rt_mem_write(c, a, &v, 4); }
static void h_vsync(Cpu *c) {      /* contract: tools/recomp/hooks.py Hle.vsync */
    rt_trace_hle(c, "VSync");
    int32_t mode = (int32_t)c->r[4];
    uint32_t fc_addr = c->hook_param, st = c->hook_param2;
    uint32_t fc = rt_lw(c, fc_addr), last_rc = rt_lw(c, st + 8);
    uint32_t rc = hwr(c, rt_lw(c, st + 4));
    uint32_t hc = (rc - last_rc) & 0xFFFF;
    if (mode < 0) { c->r[2] = fc; return; }
    if (mode == 1) { c->r[2] = hc; return; }
    uint32_t last_frame = rt_lw(c, st + 0xC);
    uint32_t target = mode > 0 ? last_frame - 1 + (uint32_t)mode : last_frame;
    while ((int32_t)fc < (int32_t)target) { io_vblank_tick(rt_io_ctx(c)); fc++; }
    io_vblank_tick(rt_io_ctx(c)); fc++;
    store_word(c, fc_addr, fc);
    store_word(c, st + 0xC, fc);
    store_word(c, st + 8, hwr(c, 0x1F801110));
    c->r[2] = hc;
}
static void h_null(Cpu *c) { rt_trace_hle(c, "Null"); c->r[2] = c->hook_param; }
static void h_enter_critical(Cpu *c) { rt_trace_hle(c, "EnterCriticalSection"); c->r[2] = c->cop0[12] & 1; c->cop0[12] &= ~1u; }
static void h_exit_critical(Cpu *c) { rt_trace_hle(c, "ExitCriticalSection"); c->cop0[12] |= 1u; }

static const struct { const char *n; RtFn f; } HOOKS[] = {
    {"IntrInit", h_intr_init},
    {"CdInit", h_cd_init}, {"CdControlB", h_cd_control_b}, {"CdRead", h_cd_read}, {"CdReadSync", h_cd_read_sync},
    {"CdSearchFile", h_cd_search_file}, {"Null", h_null}, {"VSync", h_vsync}, {"CdControl", h_cd_control}, {"CdSync", h_cd_sync}, {"EnterCriticalSection", h_enter_critical}, {"ExitCriticalSection", h_exit_critical},
};

void rt_hle_reopen_files(Cpu *c) { rt_disc_reopen(g_disc); io_cd_reopen(rt_io_ctx(c)); }       /* children of run_game --branch: own file offsets for the disc */
void rt_hle_set_card(const char *path) { g_card = card_new(path); }
void rt_hle_card_set_path(const char *path) { card_set_path(g_card, path); }
int rt_hle_card_save(void) { return card_save(g_card); }
const uint8_t *rt_hle_card_image(void) { return g_card ? card_image(g_card) : NULL; }

static void ev_reset(void);
static void serial_out(int ch);
int rt_hle_init(const char *disc_bin) {
    io_serial_hook = serial_out;
    ev_reset();
    rt_io_set_disc(disc_bin);
    rt_set_bios_handler(bios_dispatch);
    g_disc = rt_disc_open(disc_bin);
    return g_disc != NULL;
}
void rt_hle_bind(uint32_t addr, const char *name, uint32_t param, uint32_t param2) {
    for (unsigned i = 0; i < sizeof HOOKS / sizeof HOOKS[0]; i++)
        if (strcmp(HOOKS[i].n, name) == 0) {
            rt_fn_register_p(addr, HOOKS[i].f, HOOKS[i].n, RT_FN_HLE, param, param2);
            return;
        }
    rt_fatal(NULL, "rt_hle_bind: no implementation for hook %s", name);
}

/* ---- BIOS call stubs (contracts mirror tools/recomp/hooks.py `bios_*`; psx-spx function names) ---- */
static uint32_t g_clear_pad, g_clear_rcnt[4] = {1, 1, 1, 0};      /* auto-acknowledge flags (C(0Ah)); timers default to 1 as in a published BIOS reimplementation, the vblank flag is set by the SDK */
static uint32_t PAD_NONE = 0xFFFFFFFFu;   /* current pad word (input provider: rt_pad_set); 0xFFFFFFFF = no buttons */
void rt_pad_set(uint32_t word) { PAD_NONE = word; }
static struct { unsigned lo, hi; uint32_t word; } g_pscript[4096];
static unsigned g_npscript, g_vblanks;
static struct { unsigned tick; uint32_t word; } *g_rep; static unsigned g_nrep, g_irep;
/* recorded input (frontend --record): lines "tick hexword" = the pad word that applies from that virtual frame on */
void rt_pad_replay_load(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) rt_fatal(NULL, "cannot open replay file %s", path);
    unsigned t, cap = 0; unsigned w;
    while (fscanf(f, "%u %x", &t, &w) == 2) { if (g_nrep == cap) { cap = cap ? cap * 2 : 1024; g_rep = realloc(g_rep, cap * sizeof *g_rep); } g_rep[g_nrep].tick = t; g_rep[g_nrep++].word = w; }
    fclose(f);
}
static unsigned g_fuzz_on, g_fuzz_first, g_fuzz_left; static uint32_t g_fuzz_state, g_fuzz_word = 0xFFFFFFFFu;
void rt_pad_fuzz(unsigned seed, unsigned first_frame) { g_fuzz_on = 1; g_fuzz_first = first_frame; g_fuzz_state = seed ? seed : 1; }
static uint32_t fuzz_rand(void) { g_fuzz_state ^= g_fuzz_state << 13; g_fuzz_state ^= g_fuzz_state >> 17; g_fuzz_state ^= g_fuzz_state << 5; return g_fuzz_state; }
/* random input after `first_frame` (soak tests): every 5-45 frames a new random combination of 0-3 buttons (start/select rarely), then a short release */
static uint32_t fuzz_word(void) {
    if (g_fuzz_left) { g_fuzz_left--; return g_fuzz_word; }
    static const int bits[] = {12, 13, 14, 15, 6, 5, 7, 4, 2, 3, 0, 1};    /* up right down left cross circle square triangle l1 r1 l2 r2 (bit map of rt_pad_script_add) */
    uint32_t w = 0xFFFFFFFFu;
    static int walk = -1; if (walk < 0) walk = getenv("PS1_FUZZ_WALK") != NULL;      /* walker: long direction holds, Cross presses, no menu / start / select / shoulder buttons (reaches more of the game than the uniform fuzz) */
    if (walk) {
        unsigned r = fuzz_rand() % 10;
        if (r < 7) { w &= ~(1u << bits[fuzz_rand() % 4]); if (fuzz_rand() % 3 == 0) w &= ~(1u << bits[fuzz_rand() % 4]); g_fuzz_left = 20 + fuzz_rand() % 100; }
        else if (r < 9) { w &= ~(1u << 6); g_fuzz_left = 3 + fuzz_rand() % 8; }                     /* cross */
        else { g_fuzz_left = 5 + fuzz_rand() % 20; }
        g_fuzz_word = w;
        return w;
    }
    if (fuzz_rand() % 5) {
        unsigned n = 1 + fuzz_rand() % 3;
        for (unsigned i = 0; i < n; i++) w &= ~(1u << bits[fuzz_rand() % 12]);
        if (fuzz_rand() % 40 == 0) w &= ~(1u << 11);                          /* start */
        if (fuzz_rand() % 60 == 0) w &= ~(1u << 8);                           /* select */
    }
    g_fuzz_word = w; g_fuzz_left = 5 + fuzz_rand() % 40;
    return w;
}
void rt_pad_script_add(const char *spec) {      /* same bit map as tools/recomp/hooks.py Hle.PAD_BITS */
    static const struct { const char *n; int bit; } B[] = {{"select", 8}, {"l3", 9}, {"r3", 10}, {"start", 11}, {"up", 12}, {"right", 13}, {"down", 14}, {"left", 15},
        {"l2", 0}, {"r2", 1}, {"l1", 2}, {"r1", 3}, {"triangle", 4}, {"circle", 5}, {"cross", 6}, {"square", 7}};
    char buf[128]; snprintf(buf, sizeof buf, "%s", spec);
    char *at = strchr(buf, '@');
    if (!at || g_npscript == 4096) rt_fatal(NULL, "bad pad script %s", spec);
    *at = 0;
    uint32_t w = 0xFFFFFFFFu;
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
        int ok = 0;
        for (unsigned i = 0; i < sizeof B / sizeof B[0]; i++) if (!strcmp(B[i].n, t)) { w &= ~(1u << B[i].bit); ok = 1; }
        if (!ok) rt_fatal(NULL, "unknown button %s", t);
    }
    unsigned lo, hi;
    if (sscanf(at + 1, "%u-%u", &lo, &hi) != 2) rt_fatal(NULL, "bad pad script %s", spec);
    g_pscript[g_npscript].lo = lo; g_pscript[g_npscript].hi = hi; g_pscript[g_npscript].word = w; g_npscript++;
}
static void store_quiet(Cpu *c, uint32_t a, uint32_t v) { rt_mem_write(c, a, &v, 4); }
static void b_pad_init(Cpu *c) {
    if (c->r[4] != 0x20000000u && c->r[4] != 0x20000001u) { c->r[2] = 0; return; }
    g_pad_dest = c->r[5];
    if (g_pad_dest) store_quiet(c, g_pad_dest, PAD_NONE);
    c->r[2] = 2;
}
static void b_pad_get(Cpu *c) { c->r[2] = PAD_NONE; if (g_pad_dest) store_quiet(c, g_pad_dest, PAD_NONE); }
static uint32_t g_padbuf[2]; static int g_pad_started;
static void b_init_pad(Cpu *c) {                     /* B(12h) InitPAD(buf1, len1, buf2, len2): the driver then fills the buffers every vblank (docs 17 / psx-spx) */
    g_padbuf[0] = c->r[4]; g_padbuf[1] = c->r[6]; c->r[2] = 1;
}
static void b_start_pad(Cpu *c) { g_pad_started = 1; c->r[2] = 1; }                          /* B(13h) StartPAD */
static void pad_buffers_update(Cpu *c) {
    if (!g_pad_started) return;
    uint8_t d1[4] = {0x00, 0x41, (uint8_t)(PAD_NONE >> 8), (uint8_t)PAD_NONE}, d2[4] = {0xFF, 0xFF, 0xFF, 0xFF};       /* port 1: digital pad (id 41h), buttons active low; port 2: no controller */
    if (g_padbuf[0]) rt_mem_write(c, g_padbuf[0], d1, 4);
    if (g_padbuf[1]) rt_mem_write(c, g_padbuf[1], d2, 4);
}
static void b_nop(Cpu *c) { (void)c; }
static void b_ret1(Cpu *c) { c->r[2] = 1; }
static void b_getchar(Cpu *c) { c->r[2] = 0x7F; }
static void b_change_clear_pad(Cpu *c) { g_clear_pad = c->r[4]; }
static void b_change_clear_rcnt(Cpu *c) {
    uint32_t t = c->r[4];
    if (t > 3) rt_fatal(c, "ChangeClearRCnt with t=%u", t);
    c->r[2] = g_clear_rcnt[t];
    g_clear_rcnt[t] = c->r[5];
}
static uint32_t hwr(Cpu *c, uint32_t a) { int ok; uint32_t v = io_read(rt_io_ctx(c), a, 4, &ok); if (!ok) rt_fatal(c, "%s", io_last_error(rt_io_ctx(c))); return v; }
static void hww(Cpu *c, uint32_t a, uint32_t v) { int ok; io_write(rt_io_ctx(c), a, 4, v, &ok); if (!ok) rt_fatal(c, "%s", io_last_error(rt_io_ctx(c))); }
static uint32_t gpu_sync(Cpu *c) { if ((hwr(c, 0x1F801814) >> 29) & 3) hww(c, 0x1F801814, 0x04000000u); return 0; }
static void b_send_gp1(Cpu *c) { hww(c, 0x1F801814, c->r[4]); }
static void b_gpu_cw(Cpu *c) { c->r[2] = gpu_sync(c); hww(c, 0x1F801810, c->r[4]); }
static void b_gpu_cwp(Cpu *c) { uint32_t src = c->r[4], n = c->r[5]; gpu_sync(c); for (uint32_t k = 0; k < n; k++) hww(c, 0x1F801810, rt_lw(c, src + 4 * k)); c->r[2] = 0; }
static void b_get_gpustat(Cpu *c) { c->r[2] = hwr(c, 0x1F801814); }
static void b_gpu_sync(Cpu *c) { c->r[2] = gpu_sync(c); }
/* ---- interrupts (decisions/0002 section 3.6, queue P3-2): virtual time advances by one frame every RT_YIELD_PERIOD taken backward
 * branches; the vblank bit is raised and, when SR.IE is set and I_STAT & I_MASK is non-zero, the handler the library registered with
 * SetCustomExitFromException is run on a private stack until it calls ReturnFromException; registers are restored afterwards. ---- */
static void biosc_chain_walk(Cpu *c);
static void irq_deliver(Cpu *c) {
    IoCtx *x = rt_io_ctx(c);
    if (g_in_irq || !g_custom_exit || !(c->cop0[12] & 1) || !io_irq_pending(x)) return;
    rt_trace_hle(c, "IRQ");
    uint32_t r[32], hi = c->hi, lo = c->lo, sr = c->cop0[12], pc = c->pc;
    memcpy(r, c->r, sizeof r);
    /* psx-spx B(19h): the argument points to a SaveState-format structure: +0 pc, +4 sp, +8 fp, +0xC..+0x28 r16..r23, +0x2C gp; the hook runs with r2 = 1 */
    uint32_t jb[12], entry;
    rt_mem_read(c, g_custom_exit, jb, sizeof jb);
    entry = jb[0];
    c->r[29] = jb[1]; c->r[30] = jb[2];
    for (int k = 0; k < 8; k++) c->r[16 + k] = jb[3 + k];
    c->r[28] = jb[11]; c->r[2] = 1;
    c->cop0[12] &= ~1u;
    g_in_irq = 1;
    unsigned interp_depth = rt_interp_depth();
    if (setjmp(g_irq_jb) == 0) { biosc_chain_walk(c); c->r[2] = 1; c->r[31] = 0xFFFFFFE0u; rt_call(c, entry); }
    g_in_irq = 0;
    rt_interp_set_depth(interp_depth);
    memcpy(c->r, r, sizeof r);
    c->hi = hi; c->lo = lo; c->cop0[12] = sr; c->pc = pc;
}
static unsigned g_max_ticks;
void rt_set_max_ticks(unsigned n) { g_max_ticks = n; }
uint32_t rt_hle_ticks(void) { return g_ticks; }
static struct { void (*fn)(Cpu *, void *); void *ud; } g_tick_hooks[4]; static unsigned g_ntick_hooks;
void rt_set_tick_hook(void (*fn)(Cpu *, void *), void *ud) { if (g_ntick_hooks == 4) rt_fatal(NULL, "too many tick hooks"); g_tick_hooks[g_ntick_hooks].fn = fn; g_tick_hooks[g_ntick_hooks++].ud = ud; }   /* up to 4, called in registration order */
#define RT_DMA_LATENCY 300                                      /* taken backward branches between a transfer start and its completion interrupt (about 1/130 of a frame) */
static int g_dma_mini; static int32_t g_dma_rest;
void rt_dma_kick(Cpu *c) {
    if (g_dma_mini || c->yield <= RT_DMA_LATENCY) return;
    g_dma_mini = 1; g_dma_rest = c->yield - RT_DMA_LATENCY; c->yield = RT_DMA_LATENCY;
}
void rt_yield(Cpu *c) {
    if (g_dma_mini) {                                           /* short slice: only the queued DMA interrupts, no frame advance */
        g_dma_mini = 0; c->yield = g_dma_rest > 0 ? g_dma_rest : 1;
        g_async_events++;
        for (int k = 0; k < 64 && io_dma_irq_next(rt_io_ctx(c)); k++) irq_deliver(c);
        g_async_events--;
        return;
    }
    c->yield = RT_YIELD_PERIOD;
    g_async_events++;
    g_ticks++;
    if (g_max_ticks && g_ticks > g_max_ticks) rt_fatal(c, "tick limit reached");
    io_vblank_tick(rt_io_ctx(c));
    for (unsigned i = 0; i < g_ntick_hooks; i++) g_tick_hooks[i].fn(c, g_tick_hooks[i].ud);   /* host frame: present, audio, input, pacing (frontend) */
    for (unsigned pi = 0; pi < g_npend; ) {                     /* asynchronous BIOS events first (same order as tools/recomp/hooks.py on_vblank); a delayed one waits its frames */
        if (g_pend[pi][2]) { g_pend[pi][2]--; pi++; continue; }
        uint32_t cls = g_pend[pi][0], spec = g_pend[pi][1];
        memmove(g_pend[pi], g_pend[pi + 1], (g_npend - pi - 1) * sizeof g_pend[0]);
        g_npend--;
        ev_deliver(c, cls, spec);
    }
    g_vblanks++;
    if (g_npscript || g_fuzz_on || g_nrep) {
        uint32_t w = 0xFFFFFFFFu; int hit = 0;
        for (unsigned i = 0; i < g_npscript; i++) if (g_vblanks >= g_pscript[i].lo && g_vblanks <= g_pscript[i].hi) { w = g_pscript[i].word; hit = 1; break; }
        if (!hit && g_nrep) { while (g_irep + 1 < g_nrep && g_rep[g_irep + 1].tick <= g_vblanks) g_irep++; if (g_rep[g_irep].tick <= g_vblanks) { w = g_rep[g_irep].word; hit = 1; } }
        if (!hit && g_fuzz_on && g_vblanks >= g_fuzz_first) w = fuzz_word();
        PAD_NONE = w;
    }
    pad_buffers_update(c);
    if (g_pad_dest) store_quiet(c, g_pad_dest, PAD_NONE);      /* pad IRQ handler stores the buttons at [button_dest] (psx-spx B(16h)) */
    io_sio_set_pad(rt_io_ctx(c), PAD_NONE);                    /* the controller port the program's own driver talks to */
    ev_deliver(c, 0xF0000001u, 0x0002u);      /* kernel VBLANK interrupt (psx-spx event class list) */
    ev_deliver(c, 0xF2000003u, 0x0002u);      /* root counter 3 = software vblank timer (assumed to fire every vblank) */
    irq_deliver(c);
    for (int k = 0, t; k < 8 && (t = io_timer_irq_next(rt_io_ctx(c))) != 0; k++) {      /* root-counter interrupts of the frame (finding 0040) */
        t--;                                                        /* the BIOS interrupt handler: an enabled source delivers its event (F200000t, spec 2) and is acknowledged when its clear flag is set (finding 0003) */
        if ((io_imask(rt_io_ctx(c)) >> (4 + t)) & 1) { ev_deliver(c, 0xF2000000u + (uint32_t)t, 0x0002u); if (g_clear_rcnt[t]) io_irq_ack(rt_io_ctx(c), 1u << (4 + t)); }
        irq_deliver(c);
    }
    for (int k = 0; k < 32 && io_sio_irq_next(rt_io_ctx(c)); k++) irq_deliver(c);      /* SIO0 ACK interrupts: one handler run per transferred byte (finding 0006) */
    for (int k = 0; k < 64 && io_dma_irq_next(rt_io_ctx(c)); k++) irq_deliver(c);      /* DMA completion interrupts, one handler run each (finding 0003) */
    for (int k = 0; k < 3 && io_cd_step(rt_io_ctx(c)); k++) irq_deliver(c);      /* more CD interrupts within the frame (sector streaming) */
    g_async_events--;
}
static void b_return_from_exception(Cpu *c) {
    if (!g_in_irq) rt_fatal(c, "ReturnFromException outside an interrupt");
    longjmp(g_irq_jb, 1);
}
static void b_set_default_exit(Cpu *c) { (void)c; g_custom_exit = 0; }
static void b_set_custom_exit(Cpu *c) { g_custom_exit = c->r[4]; }
/* ---- BIOS events (mirror of tools/recomp/hooks.py; psx-spx "BIOS Event Functions") ---- */
typedef struct { int used; uint32_t cls, spec, mode, func; int status; } Ev;   /* status: 0 disabled, 1 enabled/busy, 2 ready */
static Ev g_ev[16];
/* slots 0..4 start as the BIOS's five CD events (class F0000003, specs 10h 20h 40h 80h 8000h); A(72h) CdRemove closes the stored handles F1000000..04 (finding 0030) */
static Ev *ev_get(uint32_t h) { uint32_t i = h - 0xF1000000u; return (i < 16 && g_ev[i].used) ? &g_ev[i] : NULL; }

/* call a guest function from a hook: registers are saved and restored (events, callbacks) */
#define EXC_STACK_TOP 0x80000FF0u
static uint32_t guest_call(Cpu *c, uint32_t addr) {
    uint32_t r[32], hi = c->hi, lo = c->lo, pc = c->pc;
    memcpy(r, c->r, sizeof r);
    if (g_async_events > 0 && !(c->r[29] >= 0x80000800u && c->r[29] <= EXC_STACK_TOP)) c->r[29] = EXC_STACK_TOP;
    c->r[4] = c->r[5] = c->r[6] = c->r[7] = 0;     /* the callback's arguments are not documented (psx-spx); same choice as Oracle.call_guest */
    c->r[31] = 0xFFFFFFE0u;
    rt_call(c, addr);
    uint32_t v0 = c->r[2];
    memcpy(c->r, r, sizeof r);
    c->hi = hi; c->lo = lo; c->pc = pc;
    return v0;
}
static void ev_deliver(Cpu *c, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < 16; i++) {
        Ev *e = &g_ev[i];
        if (e->used && e->status == 1 && e->cls == cls && e->spec == spec) {
            if (e->mode == 0x1000) { if (e->func) guest_call(c, e->func); }
            else e->status = 2;
        }
    }
}
static void ev_reset(void) {
    static const uint16_t specs[5] = {0x10, 0x20, 0x40, 0x80, 0x8000};
    memset(g_ev, 0, sizeof g_ev);
    for (int i = 0; i < 5; i++) g_ev[i] = (Ev){1, 0xF0000003u, specs[i], 0x2000, 0, 1};      /* the BIOS's own CD events */
}
static void b_open_event(Cpu *c) {
    for (int i = 0; i < 16; i++) if (!g_ev[i].used) {
        g_ev[i] = (Ev){1, c->r[4], c->r[5], c->r[6], c->r[7], 0};
        c->r[2] = 0xF1000000u + (uint32_t)i;
        if (getenv("PS1_EVLOG")) fprintf(stderr, "[ev] tick %u OpenEvent(cls %08x spec %04x mode %04x func %08x) -> slot %d ra=%08x\n", g_ticks, c->r[4], c->r[5], c->r[6], c->r[7], i, c->r[31]);
        return;
    }
    c->r[2] = 0xFFFFFFFFu;
}
static void b_cd_remove(Cpu *c) { for (int i = 0; i < 5; i++) g_ev[i].used = 0; (void)c; }
static void b_close_event(Cpu *c) { Ev *e = ev_get(c->r[4]); if (e) e->used = 0; c->r[2] = 1; }
static void b_enable_event(Cpu *c) { Ev *e = ev_get(c->r[4]); if (e) e->status = 1; c->r[2] = 1; }
static void b_disable_event(Cpu *c) { Ev *e = ev_get(c->r[4]); if (e) e->status = 0; c->r[2] = 1; }
static void b_test_event(Cpu *c) { Ev *e = ev_get(c->r[4]); if (e && e->status == 2) { e->status = 1; c->r[2] = 1; } else c->r[2] = 0; }
static void b_wait_event(Cpu *c) {
    uint32_t h = c->r[4];
    Ev *e = ev_get(h);
    if (getenv("PS1_EVLOG")) fprintf(stderr, "[ev] tick %u WaitEvent(%08x) %s cls %08x spec %04x mode %04x status %d ra=%08x\n", g_ticks, h, e ? "found" : "unknown", e ? e->cls : 0, e ? e->spec : 0, e ? e->mode : 0, e ? e->status : -1, c->r[31]);
    if (!e || e->status == 0) { c->r[2] = 0; return; }
    for (int k = 0; k < 100000; k++) {
        if (e->status == 2) { e->status = 1; c->r[2] = 1; return; }
        rt_yield(c);
    }
    rt_fatal(c, "WaitEvent(%08x) never became ready", h);
}
static void b_deliver_event(Cpu *c) { ev_deliver(c, c->r[4], c->r[5]); }
static void b_undeliver_event(Cpu *c) {
    for (int i = 0; i < 16; i++) { Ev *e = &g_ev[i]; if (e->used && e->status == 2 && e->mode == 0x2000 && e->cls == c->r[4] && e->spec == c->r[5]) e->status = 1; }
}
static void bios_dispatch_name(Cpu *c, char t, unsigned n, RtFn *f) {
    (void)c;
    *f = NULL;
    if (t == 'A' && n == 0x3B) *f = b_getchar;                               /* getchar: no console input (the reference BIOS answers 7Fh) */
    else if (t == 'B' && (n == 0x47 || n == 0x48)) *f = b_ret1;                  /* AddDrv / DelDrv: the device list is not modelled beyond the "bu" entry; success */
    else if (t == 'B' && n == 0x12) *f = b_init_pad;
    else if (t == 'B' && n == 0x13) *f = b_start_pad;
    else if (t == 'B' && n == 0x15) *f = b_pad_init;
    else if (t == 'B' && n == 0x16) *f = b_pad_get;
    else if (t == 'B' && n == 0x14) *f = b_nop;
    else if (t == 'B' && n == 0x5B) *f = b_change_clear_pad;
    else if (t == 'C' && n == 0x0A) *f = b_change_clear_rcnt;
    else if (t == 'A' && n == 0x44) *f = b_nop;
    else if (t == 'B' && n == 0x35) *f = h_file_write;
    else if (t == 'A' && n == 0x72) *f = b_cd_remove;
    else if (t == 'B' && n == 0x07) *f = b_deliver_event;
    else if (t == 'B' && n == 0x08) *f = b_open_event;
    else if (t == 'B' && n == 0x09) *f = b_close_event;
    else if (t == 'B' && n == 0x0A) *f = b_wait_event;
    else if (t == 'B' && n == 0x0B) *f = b_test_event;
    else if (t == 'B' && n == 0x0C) *f = b_enable_event;
    else if (t == 'B' && n == 0x0D) *f = b_disable_event;
    else if (t == 'B' && n == 0x20) *f = b_undeliver_event;
    else if (t == 'B' && n == 0x17) *f = b_return_from_exception;
    else if (t == 'B' && n == 0x18) *f = b_set_default_exit;
    else if (t == 'B' && n == 0x19) *f = b_set_custom_exit;
    else if (t == 'A' && n == 0x48) *f = b_send_gp1;
    else if (t == 'A' && n == 0x49) *f = b_gpu_cw;
    else if (t == 'A' && n == 0x4A) *f = b_gpu_cwp;
    else if (t == 'A' && n == 0x4D) *f = b_get_gpustat;
    else if (t == 'A' && n == 0x4E) *f = b_gpu_sync;
}
static void h_init_heap_bios(Cpu *c) { (void)c; }
/* BIOS A(3Ch) putchar, A(3Eh) puts, A(3Fh) printf: appended to the captured console output (psx-spx "Kernel Standard Functions"; the C runtime only, no game executable uses them).
 * printf supports %d %i %u %x %X %c %s %p %% with flags -/0, width and the l/h modifiers; the arguments are a1..a3 and then the stack words from sp+16. */
static void out_ch(char ch) { if (g_outn < sizeof g_out) g_out[g_outn++] = ch; }
static void serial_out(int ch) { out_ch((char)ch); }          /* serial port 1 transmit = console capture */
static void h_stdio(Cpu *c, unsigned n) {
    if (n == 0x3C) { out_ch((char)c->r[4]); c->r[2] = c->r[4] & 0xFF; return; }
    uint32_t p = c->r[4];
    if (n == 0x3E) { for (char ch; (ch = (char)rt_lbu(c, p)) != 0; p++) out_ch(ch); out_ch('\n'); c->r[2] = 1; return; }
    unsigned argi = 0;
    uint32_t sp = c->r[29];
    #define NEXTARG() (argi < 3 ? c->r[5 + argi++] : rt_lw(c, sp + 16 + 4 * (argi++ - 3)))
    size_t start = g_outn;
    for (char ch; (ch = (char)rt_lbu(c, p)) != 0; p++) {
        if (ch != '%') { out_ch(ch); continue; }
        int left = 0, zero = 0, alt = 0, width = 0, prec = -1;
        for (;; ) { char f = (char)rt_lbu(c, p + 1); if (f == '-') left = 1; else if (f == '0') zero = 1; else if (f == '#') alt = 1; else if (f == '+' || f == ' ') { } else break; p++; }
        if (rt_lbu(c, p + 1) == '*') { width = (int)NEXTARG(); p++; if (width < 0) { left = 1; width = -width; } }
        else while (rt_lbu(c, p + 1) >= '0' && rt_lbu(c, p + 1) <= '9') { width = width * 10 + (int)(rt_lbu(c, p + 1) - '0'); p++; }
        if (rt_lbu(c, p + 1) == '.') {
            p++; prec = 0;
            if (rt_lbu(c, p + 1) == '*') { prec = (int)NEXTARG(); p++; }
            else while (rt_lbu(c, p + 1) >= '0' && rt_lbu(c, p + 1) <= '9') { prec = prec * 10 + (int)(rt_lbu(c, p + 1) - '0'); p++; }
        }
        while (rt_lbu(c, p + 1) == 'l' || rt_lbu(c, p + 1) == 'h') p++;
        char cv = (char)rt_lbu(c, ++p);
        char buf[600]; int len = 0;
        if (cv == '%') { buf[len++] = '%'; }
        else if (cv == 'c') { buf[len++] = (char)NEXTARG(); }
        else if (cv == 's') { uint32_t sp2 = NEXTARG(); for (char q; (q = (char)rt_lbu(c, sp2)) != 0 && len < 560 && (prec < 0 || len < prec); sp2++) buf[len++] = q; }
        else if (cv == 'd' || cv == 'i') { int32_t v = (int32_t)NEXTARG(); len = snprintf(buf, sizeof buf, "%d", v); }
        else if (cv == 'u') { len = snprintf(buf, sizeof buf, "%u", NEXTARG()); }
        else if (cv == 'x') { uint32_t v = NEXTARG(); len = snprintf(buf, sizeof buf, alt && v ? "0x%x" : "%x", v); }
        else if (cv == 'X') { uint32_t v = NEXTARG(); len = snprintf(buf, sizeof buf, alt && v ? "0X%X" : "%X", v); }
        else if (cv == 'p') { len = snprintf(buf, sizeof buf, "%X", NEXTARG()); }
        else { out_ch('%'); out_ch(cv); continue; }
        if (alt && (cv == 'x' || cv == 'X') && zero && !left && width > len && len > 2) {       /* %#010x: zeros go between the prefix and the digits */
            int pad0 = width - len; memmove(buf + 2 + pad0, buf + 2, (size_t)(len - 2)); memset(buf + 2, '0', (size_t)pad0); len = width;
        }
        int pad = width > len ? width - len : 0;
        if (!left) for (int k = 0; k < pad; k++) out_ch(zero && cv != 's' && cv != 'c' ? '0' : ' ');
        for (int k = 0; k < len; k++) out_ch(buf[k]);
        if (left) for (int k = 0; k < pad; k++) out_ch(' ');
    }
    #undef NEXTARG
    c->r[2] = (uint32_t)(g_outn - start);
}
#include "biosc.inc"
static unsigned g_bios_seen[3][256], g_bios_total[3][256];
static void bios_summary(void) { fprintf(stderr, "[bioslog] calls:"); for (int t = 0; t < 3; t++) for (int n = 0; n < 256; n++) if (g_bios_total[t][n]) fprintf(stderr, " %c(%02X)x%u", 'A' + t, n, g_bios_total[t][n]); fprintf(stderr, "\n"); }
static void bios_dispatch(Cpu *c, char t, unsigned n) {
    if (t == 'A' && n <= 5) { t = 'B'; n += 0x32; }              /* A(00h..05h) FileOpen/Seek/Read/Write/Close/Ioctl are the same functions as B(32h..37h) (psx-spx) */
    static int bl = -1; if (bl < 0) { bl = getenv("PS1_BIOSLOG") != NULL; if (bl) atexit(bios_summary); }       /* debug aid: the first call of every BIOS function with its arguments, and a call count summary at exit */
    if (bl && n < 256) { g_bios_total[t - 'A'][n]++; if (!g_bios_seen[t - 'A'][n]++) fprintf(stderr, "[bioslog] first %c(%02X) a0-a3 %08x %08x %08x %08x from ra %08x\n", t, n, c->r[4], c->r[5], c->r[6], c->r[7], c->r[31]); }
    { static long from = -2; if (from == -2) from = getenv("PS1_BIOSFROM") ? atol(getenv("PS1_BIOSFROM")) : -1; if (from >= 0 && (long)g_ticks >= from) fprintf(stderr, "[bios] tick %u %c(%02X) a0-a3 %08x %08x %08x %08x ra %08x\n", g_ticks, t, n, c->r[4], c->r[5], c->r[6], c->r[7], c->r[31]); }      /* debug aid: every BIOS call from that virtual frame on */
    COV(RT_COV_BIOS, (t - 'A') * 256 + n);
    char name[16];
    snprintf(name, sizeof name, "BIOS_%c%02X", t, n);
    rt_trace_hle(c, name);
    if (biosc_call(c, t, n)) return;
    if (t == 'A' && (n == 0x3C || n == 0x3E || n == 0x3F)) { h_stdio(c, n); return; }
    if (t == 'A' && n == 0x39) { (void)h_init_heap_bios; return; }
    if (t == 'A' && n == 0x43) {
        g_exec[0] = c->r[4]; g_exec[1] = c->r[5]; g_exec[2] = c->r[6];
        if (!g_chain) longjmp(g_stop, 1);
        /* psx-spx A(43h): zero-fill the memfill region, SP/FP = base+offset when base != 0, GP from the header, entry called with p1, p2 in r4/r5;
         * when the executable returns the registers are restored and r2 = 1 (tools/recomp/hooks.py do_execute mirrors this) */
        uint32_t hdr = c->r[4], p1 = c->r[5], p2 = c->r[6];
        uint32_t entry = rt_lw(c, hdr), gp = rt_lw(c, hdr + 4), fa = rt_lw(c, hdr + 0x18), fn = rt_lw(c, hdr + 0x1C), sb = rt_lw(c, hdr + 0x20), so = rt_lw(c, hdr + 0x24);
        for (uint32_t k = 0; k < fn; k++) { uint8_t z = 0; rt_mem_write(c, fa + k, &z, 1); }
        char nm[40]; size_t ni = 0;                                  /* the launcher keeps the loaded program's name at 0x80010000 (finding 0003) */
        for (; ni < sizeof nm - 1; ni++) { char ch = (char)rt_lbu(c, 0x80010000u + (uint32_t)ni); if (!ch) break; nm[ni] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch; }
        nm[ni] = 0;
        snprintf(g_last_file, sizeof g_last_file, "%s", nm);
        if (getenv("PS1_HANDOFF")) { uint8_t hb[0x100]; rt_mem_read(c, 0x80010000u, hb, sizeof hb); FILE *hf = fopen(getenv("PS1_HANDOFF"), "ab"); if (hf) { fprintf(hf, "%s\n", nm); for (unsigned k = 0; k < sizeof hb; k++) fprintf(hf, "%02x", hb[k]); fprintf(hf, "\n"); fclose(hf); } }
        if (getenv("PS1_HANDOFF_PATCH") && !strcmp(g_last_file, "FB2.EXE")) {              /* debug aid: PS1_HANDOFF_PATCH="18=3,28=0" sets words of the hand-off block (offset=value, hex) before FB2 starts: +0x18 is the 0-based level (finding 0034) */
            char spec[256]; snprintf(spec, sizeof spec, "%s", getenv("PS1_HANDOFF_PATCH"));
            for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) { unsigned off, val; if (sscanf(t, "%x=%x", &off, &val) == 2) { uint32_t w = val; rt_mem_write(c, 0x80010000u + off, &w, 4); } }
        }
        Module *m = NULL;
        for (unsigned i = 0; i < g_nmod; i++) if (strcmp(g_mod[i].name, g_last_file) == 0) m = &g_mod[i];
        if (!m) rt_fatal(c, "DoExecute: no recompiled module for %s", g_last_file);
        io_gpulog_phase(g_last_file);
        rt_fn_reset_above(0x20000u);
        rt_code_clear_above(0x80020000u);
        m->code(); m->hooks(); m->reg();
        uint32_t r[32], hi = c->hi, lo = c->lo;
        memcpy(r, c->r, sizeof r);
        c->r[28] = gp;
        if (sb) c->r[29] = c->r[30] = sb + so;
        c->r[4] = p1; c->r[5] = p2; c->r[6] = 0; c->r[7] = 0;
        c->r[31] = 0xFFFFFFE0u;
        rt_call(c, entry);
        memcpy(c->r, r, sizeof r);
        c->hi = hi; c->lo = lo;
        c->r[2] = 1;
        return;
    }
    if (!(t == 'B' && n == 0x35 && c->r[4] == 1) && g_card) {      /* memory-card layer (runtime/card.c) */
        uint32_t a[4] = {c->r[4], c->r[5], c->r[6], c->r[7]}, ret, ev[8][2];
        int nev;
        uint8_t *ram = rt_ram_ptr(c);
        if (card_call(g_card, ram, t, n, a, &ret, ev, &nev)) {
            c->r[2] = ret;
            for (int k = 0; k < nev && g_npend < 64; k++) { g_pend[g_npend][0] = ev[k][0]; g_pend[g_npend][1] = ev[k][1]; g_pend[g_npend][2] = (uint32_t)card_event_delay(g_card); g_npend++; }      /* _card_load takes 3 frames in the reference BIOS (3 sectors, one per frame); an async read or write one frame per 128-byte sector */   /* delivered at the next virtual frame */
            return;
        }
    }
    RtFn f;
    bios_dispatch_name(c, t, n, &f);
    if (!f) rt_fatal(c, "BIOS %c(%02X) is not implemented (called from %08x)", t, n, c->r[31]);
    if (f == h_file_write) {
        uint32_t fd = c->r[4], src = c->r[5], len = c->r[6];
        if (fd != 1) rt_fatal(c, "FileWrite to fd %u (only stdout is proven used)", fd);
        for (uint32_t k = 0; k < len; k++) { uint8_t b = (uint8_t)rt_lbu(c, src + k); if (g_outn < sizeof g_out) g_out[g_outn++] = (char)b; }
        stop_text_check(c);
        c->r[2] = len;
        return;
    }
    f(c);
}

int rt_run_entry(Cpu *c, uint32_t entry) {
    if (setjmp(g_stop)) return 1;
    rt_call(c, entry);
    return 0;
}
const char *rt_hle_stdout(size_t *n) { *n = g_outn; return g_out; }
void rt_hle_last_exec(uint32_t out[3]) { memcpy(out, g_exec, sizeof g_exec); }
