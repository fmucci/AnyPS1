/* Fail-loud handler, guest exceptions, COP0 subset and the GTE placeholder (queue P2-8). Stubs are listed in docs/stubs.md. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#ifdef RT_CALLTRACE
uint32_t rt_ct[4096];
uint32_t rt_hle_ticks(void);
void rt_pclog(Cpu *c, uint32_t pc) {
    static uint32_t list[32]; static int n = -1;
    if (n < 0) { n = 0; const char *e = getenv("PS1_PCLOG"); if (e) { char *s2 = strdup(e); for (char *t = strtok(s2, ","); t && n < 32; t = strtok(NULL, ",")) list[n++] = (uint32_t)strtoul(t, NULL, 16); } }
    for (int i = 0; i < n; i++) if (list[i] == pc) { fprintf(stderr, "[pc] frame %u pc=%08x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x s0=%08x s1=%08x\n", rt_hle_ticks(), pc, c->r[4], c->r[5], c->r[6], c->r[7], c->r[31], c->r[29], c->r[16], c->r[17]); return; }
}
unsigned rt_cti;
uint32_t rt_break_pc = 0xFFFFFFFFu;
uint32_t rt_hle_ticks(void);
void rt_break_hit(Cpu *c) { const char *t = getenv("PS1_BREAK_TICK"); if (t && rt_hle_ticks() < (uint32_t)atoi(t)) return; rt_fatal(c, "break: function %08x entered at tick %u (a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x)", rt_break_pc, rt_hle_ticks(), c->r[4], c->r[5], c->r[6], c->r[7], c->r[31]); }
#endif
static RtFatalFn g_fatal;
void rt_set_fatal_handler(RtFatalFn fn) { g_fatal = fn; }

void rt_fatal(const Cpu *c, const char *fmt, ...) {
    char msg[512], full[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* generated code only records c->pc when a fragment/function stops, so this is the last RECORDED pc, not the faulting one */
    snprintf(full, sizeof full, "PS1 FATAL: %s [last recorded pc=%08x ra=%08x]", msg, c ? c->pc : 0, c ? c->r[31] : 0);
#ifdef RT_CALLTRACE
    fprintf(stderr, "recent function entries (oldest first):");
    { unsigned n = getenv("PS1_RING") ? (unsigned)atoi(getenv("PS1_RING")) : 64; if (n > 4096) n = 4096; for (unsigned i = 0; i < n; i++) fprintf(stderr, " %08x", rt_ct[(rt_cti + 4096 - n + i) & 4095]); }
    fprintf(stderr, "\n");
#endif
    if (g_fatal) g_fatal(full);
    fprintf(stderr, "%s\n", full);
    abort();
}

void rt_exception(Cpu *c, uint32_t pc, uint32_t code, uint32_t arg) {
    COV(RT_COV_EXC, code);
    if (code == 8 && (c->r[4] == 1 || c->r[4] == 2)) {         /* psx-spx "BIOS Exception handling": syscall 1 = EnterCriticalSection (v0 = previous IE), 2 = ExitCriticalSection; execution continues after the instruction */
        if (c->r[4] == 1) { c->r[2] = c->cop0[12] & 1; c->cop0[12] &= ~1u; } else c->cop0[12] |= 1u;
        c->exc = 0;
        return;
    }
    const char *n = code == 8 ? "syscall" : code == 9 ? "break" : code == 12 ? "arithmetic overflow" : "other";
    c->exc = code; c->exc_pc = pc; c->exc_arg = arg;
    rt_fatal(c, "guest exception %u (%s) at pc=%08x arg=%05x: no handler is implemented (BIOS layer is queue P3-1)", code, n, pc, arg);
}

/* COP0: only SR(12), Cause(13) read, EPC(14) are accepted; the scan of the executables cannot tell which registers are really
 * used (it also matches data words), so every other register aborts until a trace proves it (finding 0018). */
uint32_t cop0_mfc0(Cpu *c, unsigned n) {
    if (n != 12 && n != 13 && n != 14) rt_fatal(c, "mfc0 of unproven COP0 register %u", n);
    return c->cop0[n];
}
void cop0_mtc0(Cpu *c, unsigned n, uint32_t v) {
    if (n != 12 && n != 14) rt_fatal(c, "mtc0 to unproven COP0 register %u (value %08x)", n, v);
    c->cop0[n] = v;
}
void cop0_rfe(Cpu *c) { c->cop0[12] = (c->cop0[12] & ~0xFu) | ((c->cop0[12] >> 2) & 0xFu); }

/* GTE: register file only (runtime/gte.c); commands abort until queue P4-1. */
#include "gte.h"
uint32_t gte_mfc2(Cpu *c, unsigned n) { return gte_core_mfc2(c->gte_d, c->gte_c, n); }
uint32_t gte_cfc2(Cpu *c, unsigned n) { return gte_core_cfc2(c->gte_d, c->gte_c, n); }
void gte_mtc2(Cpu *c, unsigned n, uint32_t v) { gte_core_mtc2(c->gte_d, c->gte_c, n, v); }
void gte_ctc2(Cpu *c, unsigned n, uint32_t v) { gte_core_ctc2(c->gte_d, c->gte_c, n, v); }
void gte_cmd(Cpu *c, uint32_t imm25) {
    if (!gte_core_cmd(c->gte_d, c->gte_c, imm25)) rt_fatal(c, "GTE command %07x is not implemented (not in the psx-spx command list)", imm25);
}

/* ---- coverage counters (docs 18): off unless env PS1_COVERAGE=FILE; FILE is written as JSON when the program ends; -DRT_COVERAGE also records the entry of every generated function ---- */
int rt_cov_on;
static unsigned long long g_cnt[RT_COV_NGROUPS][1024];
static unsigned char g_fn[0x200000 / 4 / 8];
static const char *const G_NAME[RT_COV_NGROUPS] = {"bios", "gp0", "gp1", "cd_cmd", "dma_chan", "mdec_cmd", "spu_keyon_voice", "io_read", "io_write", "irq", "sio", "exception", "interp_entry", "cd_sector", "card_call"};

void rt_cov(int group, unsigned id) { if (group >= 0 && group < RT_COV_NGROUPS && id < 1024) g_cnt[group][id]++; }
void rt_cov_fn(uint32_t a) { uint32_t i = (a & 0x1FFFFFu) >> 2; g_fn[i >> 3] |= (unsigned char)(1u << (i & 7)); }

static void report(void) {
    const char *p = getenv("PS1_COVERAGE");
    FILE *f = p ? fopen(p, "w") : NULL;
    if (!f) return;
    fprintf(f, "{\n");
    for (int g = 0; g < RT_COV_NGROUPS; g++) {
        fprintf(f, "  \"%s\": {", G_NAME[g]);
        int first = 1;
        for (unsigned i = 0; i < 1024; i++) if (g_cnt[g][i]) { fprintf(f, "%s\"%u\": %llu", first ? "" : ", ", i, g_cnt[g][i]); first = 0; }
        fprintf(f, "},\n");
    }
    fprintf(f, "  \"functions\": [");
    int first = 1;
    for (uint32_t i = 0; i < 0x200000 / 4; i++) if (g_fn[i >> 3] >> (i & 7) & 1) { fprintf(f, "%s%u", first ? "" : ", ", 0x80000000u + i * 4); first = 0; }
    fprintf(f, "]\n}\n");
    fclose(f);
}
void rt_cov_init(void) { if (getenv("PS1_COVERAGE")) { rt_cov_on = 1; atexit(report); } }
