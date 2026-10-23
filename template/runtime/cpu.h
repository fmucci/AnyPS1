/* Guest CPU state and the small helper API that generated code (tools/recomp/emit.py) is written against.
 * Memory, exception, COP0 and GTE entry points are declared here and implemented by the runtime (P2-8, P4-1) or, for the
 * instruction-level tests, by tests/recomp/harness_support.c. Semantics: docs/decisions/0002-recompiler-c-emission.md. */
#ifndef PS1_CPU_H
#define PS1_CPU_H
#include <stdint.h>

typedef struct Cpu {
    uint32_t r[32];          /* r[0] is never written by generated code */
    uint32_t hi, lo;
    uint32_t pc;             /* where a fragment/function stopped (host dispatch uses it) */
    uint32_t cop0[32];
    uint32_t gte_d[32];      /* COP2 data registers (cop2r0-31) */
    uint32_t gte_c[32];      /* COP2 control registers (cop2r32-63) */
    uint32_t exc, exc_pc, exc_arg;  /* set by rt_exception; 0 = none */
    uint32_t gte_ncmds, gte_last_cmd;
    uint32_t hook_param, hook_param2;
    int32_t yield;           /* taken backward direct branches left until the next virtual vblank (RT_YIELD_PERIOD) */     /* set by rt_call before running an HLE hook: the `param` column of tools/recomp/hooks.csv */
    void *host;              /* runtime-private (memory window, devices) */
} Cpu;

/* guest memory: aligned accesses only (the runtime must fail loudly on misalignment / unmapped addresses) */
uint32_t rt_lw(Cpu *c, uint32_t a);
uint32_t rt_lh(Cpu *c, uint32_t a);   /* sign-extended */
uint32_t rt_lhu(Cpu *c, uint32_t a);
uint32_t rt_lb(Cpu *c, uint32_t a);   /* sign-extended */
uint32_t rt_lbu(Cpu *c, uint32_t a);
void rt_sw(Cpu *c, uint32_t a, uint32_t v);
void rt_sh(Cpu *c, uint32_t a, uint32_t v);
void rt_sb(Cpu *c, uint32_t a, uint32_t v);

/* exceptions that the original BIOS turns into SystemErrorUnresolvedException: 8 syscall, 9 break (arg = 20-bit code), 12 overflow */
void rt_exception(Cpu *c, uint32_t pc, uint32_t code, uint32_t arg);

uint32_t cop0_mfc0(Cpu *c, unsigned n);
void cop0_mtc0(Cpu *c, unsigned n, uint32_t v);
void cop0_rfe(Cpu *c);

uint32_t gte_mfc2(Cpu *c, unsigned n);
uint32_t gte_cfc2(Cpu *c, unsigned n);
void gte_mtc2(Cpu *c, unsigned n, uint32_t v);
void gte_ctc2(Cpu *c, unsigned n, uint32_t v);
void gte_cmd(Cpu *c, uint32_t imm25);

/* arithmetic helpers (portable: no reliance on implementation-defined signed shifts) */
static inline uint32_t rt_sra(uint32_t v, unsigned n) {
    return n ? ((v >> n) | ((v >> 31) ? ~(0xFFFFFFFFu >> n) : 0u)) : v;
}
/* psx-spx "Multiply/divide": no exception; divide by zero and 0x80000000 / -1 return fixed values */
static inline void rt_div(Cpu *c, uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sb == 0) { c->lo = sa < 0 ? 1u : 0xFFFFFFFFu; c->hi = a; }
    else if (a == 0x80000000u && sb == -1) { c->lo = 0x80000000u; c->hi = 0; }
    else { c->lo = (uint32_t)(sa / sb); c->hi = (uint32_t)(sa % sb); }
}
static inline void rt_divu(Cpu *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->lo = 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}

/* optional call tracing for debugging fatal stops: compile with -DRT_CALLTRACE; every generated function records its entry in a ring buffer */
#ifdef RT_CALLTRACE
extern uint32_t rt_ct[4096];
extern unsigned rt_cti;
extern uint32_t rt_break_pc;                                   /* debug aid: env PS1_BREAK=hexaddr stops the first time that function is entered */
void rt_break_hit(struct Cpu *c);
void rt_pclog(struct Cpu *c, uint32_t pc);                   /* debug aid: env PS1_PCLOG=addr,addr prints a0-a3/ra/sp every time one of those functions is entered (format of the reference host's --pcs) */
#define RT_ENTER(a) (rt_ct[rt_cti++ & 4095] = (a), rt_pclog(c, (a)), (void)((a) == rt_break_pc ? (rt_break_hit(c), 0) : 0))
#elif defined(RT_COVERAGE)
void rt_cov_fn(uint32_t a);
#define RT_ENTER(a) rt_cov_fn(a)
#else
#define RT_ENTER(a) ((void)0)
#endif
#endif
