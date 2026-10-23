/* Interpreter tier (finding 0004): runs guest code that has no generated function, mostly code loaded into RAM at run time (overlays) that no static pass could name.
 * It is entered from rt_call (runtime/dispatch.c) when the target has no registered function; it executes MIPS-I (+COP0/COP2) from guest RAM on the same Cpu and memory API as the
 * generated code, calls registered functions (generated code, HLE hooks, BIOS entry points) through rt_call, and returns to its caller when the interpreted code returns to the
 * address the caller left in $ra. Virtual time follows the generated code's rule: a taken direct branch or `j` to a lower-or-equal address counts one yield tick.
 * Load delay slots and branch delay slots are modelled exactly. Debug: PS1_INTERPLOG=1 prints the first entry address of every interpreted region. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

typedef struct { int reg; uint32_t val; int wrote; } Pending;     /* load whose value becomes visible after the next instruction; `wrote` = register written by the instruction that produced this record (a pending load into it is dropped) */

static unsigned long long g_interp_insns;
static unsigned g_depth;                                  /* nesting of interpreter sessions; a longjmp out of interpreted code (ReturnFromException leaves the interrupt handler that way) must restore it */
unsigned rt_interp_depth(void) { return g_depth; }
void rt_interp_set_depth(unsigned d) { g_depth = d; }
static uint32_t g_ring[1024]; static unsigned g_ringi;          /* the last interpreted pcs (debug aid: PS1_INTERPRING=N printed after the run) */
void rt_interp_dump(unsigned n) { if (n > 1024) n = 1024; fprintf(stderr, "[interp ring] last %u interpreted pcs (oldest first):", n); for (unsigned i = 0; i < n; i++) fprintf(stderr, " %08x", g_ring[(g_ringi + 1024 - n + i) & 1023]); fprintf(stderr, "\n"); }
unsigned long long rt_interp_count(void) { return g_interp_insns; }

#ifdef RT_INTERP_TEST                                   /* tests/recomp/run_diff_interp.py: code comes from the test case, execution stops when the pc leaves the fragment or an exception is raised */
uint32_t rt_interp_test_fetch(uint32_t pc);
int rt_interp_test_out(uint32_t pc);
static uint32_t fetch(Cpu *c, uint32_t pc) { (void)c; return rt_interp_test_fetch(pc); }
#else
static uint32_t fetch(Cpu *c, uint32_t pc) {
    uint8_t *ram = rt_ram_ptr(c);
    uint32_t p = pc & 0x1FFFFFFFu;
    if (p >= 0x200000u) rt_fatal(c, "interpreter: instruction fetch outside RAM at %08x", pc);
    uint32_t w; memcpy(&w, ram + p, 4); return w;
}
#endif

static void setreg(Cpu *c, Pending *nl, unsigned r, uint32_t v) { if (r) { c->r[r] = v; nl->wrote = (int)r; } }
static void setload(Pending *nl, unsigned r, uint32_t v) { if (r) { nl->reg = (int)r; nl->val = v; nl->wrote = (int)r; } }

/* executes one non-branch instruction; `pl` is the load pending from the previous instruction (already known to the caller), the new pending load is returned in `nl` */
static void exec_plain(Cpu *c, uint32_t pc, uint32_t w, const Pending *pl, Pending *nl) {
    unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    uint16_t imm = (uint16_t)w; int32_t simm = (int16_t)imm;
    uint32_t *r = c->r;
    nl->reg = -1; nl->wrote = -1;
    c->pc = pc;
    #define EA() (r[rs] + (uint32_t)simm)
    if (op == 0) {
        switch (fn) {
        case 0x00: if (w) setreg(c, nl, rd, r[rt] << sa); break;
        case 0x02: setreg(c, nl, rd, r[rt] >> sa); break;
        case 0x03: setreg(c, nl, rd, rt_sra(r[rt], sa)); break;
        case 0x04: setreg(c, nl, rd, r[rt] << (r[rs] & 31)); break;
        case 0x06: setreg(c, nl, rd, r[rt] >> (r[rs] & 31)); break;
        case 0x07: setreg(c, nl, rd, rt_sra(r[rt], r[rs] & 31)); break;
        case 0x0C: rt_exception(c, pc, 8, (w >> 6) & 0xFFFFF); break;
        case 0x0D: rt_exception(c, pc, 9, (w >> 6) & 0xFFFFF); break;
        case 0x10: setreg(c, nl, rd, c->hi); break;
        case 0x11: c->hi = r[rs]; break;
        case 0x12: setreg(c, nl, rd, c->lo); break;
        case 0x13: c->lo = r[rs]; break;
        case 0x18: { int64_t p = (int64_t)(int32_t)r[rs] * (int64_t)(int32_t)r[rt]; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); break; }
        case 0x19: { uint64_t p = (uint64_t)r[rs] * (uint64_t)r[rt]; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); break; }
        case 0x1A: rt_div(c, r[rs], r[rt]); break;
        case 0x1B: rt_divu(c, r[rs], r[rt]); break;
        case 0x20: { uint32_t s = r[rs] + r[rt]; if (((r[rs] ^ s) & (r[rt] ^ s)) >> 31) rt_exception(c, pc, 12, 0); setreg(c, nl, rd, s); break; }
        case 0x21: setreg(c, nl, rd, r[rs] + r[rt]); break;
        case 0x22: { uint32_t s = r[rs] - r[rt]; if (((r[rs] ^ r[rt]) & (r[rs] ^ s)) >> 31) rt_exception(c, pc, 12, 0); setreg(c, nl, rd, s); break; }
        case 0x23: setreg(c, nl, rd, r[rs] - r[rt]); break;
        case 0x24: setreg(c, nl, rd, r[rs] & r[rt]); break;
        case 0x25: setreg(c, nl, rd, r[rs] | r[rt]); break;
        case 0x26: setreg(c, nl, rd, r[rs] ^ r[rt]); break;
        case 0x27: setreg(c, nl, rd, ~(r[rs] | r[rt])); break;
        case 0x2A: setreg(c, nl, rd, (int32_t)r[rs] < (int32_t)r[rt]); break;
        case 0x2B: setreg(c, nl, rd, r[rs] < r[rt]); break;
        default: rt_fatal(c, "interpreter: illegal SPECIAL word %08x at %08x", w, pc);
        }
    } else switch (op) {
    case 0x08: { uint32_t s = r[rs] + (uint32_t)simm; if (((r[rs] ^ s) & ((uint32_t)simm ^ s)) >> 31) rt_exception(c, pc, 12, 0); setreg(c, nl, rt, s); break; }
    case 0x09: setreg(c, nl, rt, r[rs] + (uint32_t)simm); break;
    case 0x0A: setreg(c, nl, rt, (int32_t)r[rs] < simm); break;
    case 0x0B: setreg(c, nl, rt, r[rs] < (uint32_t)simm); break;
    case 0x0C: setreg(c, nl, rt, r[rs] & imm); break;
    case 0x0D: setreg(c, nl, rt, r[rs] | imm); break;
    case 0x0E: setreg(c, nl, rt, r[rs] ^ imm); break;
    case 0x0F: setreg(c, nl, rt, (uint32_t)imm << 16); break;
    case 0x10:
        if (rs == 0) setload(nl, rt, cop0_mfc0(c, rd));
        else if (rs == 4) cop0_mtc0(c, rd, r[rt]);
        else if (rs >= 0x10 && fn == 0x10) cop0_rfe(c);
        else rt_fatal(c, "interpreter: illegal COP0 word %08x at %08x", w, pc);
        break;
    case 0x12:
        if (w & (1u << 25)) gte_cmd(c, w & 0x1FFFFFF);
        else if (rs == 0) setload(nl, rt, gte_mfc2(c, rd));
        else if (rs == 2) setload(nl, rt, gte_cfc2(c, rd));
        else if (rs == 4) gte_mtc2(c, rd, r[rt]);
        else if (rs == 6) gte_ctc2(c, rd, r[rt]);
        else rt_fatal(c, "interpreter: illegal COP2 word %08x at %08x", w, pc);
        break;
    case 0x20: setload(nl, rt, rt_lb(c, EA())); break;
    case 0x21: setload(nl, rt, rt_lh(c, EA())); break;
    case 0x22: {                                                         /* lwl */
        uint32_t a = EA(), k = a & 3, m = rt_lw(c, a & ~3u);
        uint32_t cur = (pl && pl->reg == (int)rt) ? pl->val : r[rt];
        setload(nl, rt, (cur & (0x00FFFFFFu >> (8 * k))) | (m << (24 - 8 * k)));
        break;
    }
    case 0x23: setload(nl, rt, rt_lw(c, EA())); break;
    case 0x24: setload(nl, rt, rt_lbu(c, EA())); break;
    case 0x25: setload(nl, rt, rt_lhu(c, EA())); break;
    case 0x26: {                                                         /* lwr */
        uint32_t a = EA(), k = a & 3, m = rt_lw(c, a & ~3u);
        uint32_t cur = (pl && pl->reg == (int)rt) ? pl->val : r[rt];
        setload(nl, rt, (cur & ~(0xFFFFFFFFu >> (8 * k))) | (m >> (8 * k)));
        break;
    }
    case 0x28: rt_sb(c, EA(), r[rt]); break;
    case 0x29: rt_sh(c, EA(), r[rt]); break;
    case 0x2A: { uint32_t a = EA(), k = a & 3, m = rt_lw(c, a & ~3u); rt_sw(c, a & ~3u, (m & (0xFFFFFF00u << (8 * k))) | (r[rt] >> (24 - 8 * k))); break; }
    case 0x2B: rt_sw(c, EA(), r[rt]); break;
    case 0x2E: { uint32_t a = EA(), k = a & 3, m = rt_lw(c, a & ~3u); rt_sw(c, a & ~3u, (m & (0x00FFFFFFu >> (24 - 8 * k))) | (r[rt] << (8 * k))); break; }
    case 0x32: gte_mtc2(c, rt, rt_lw(c, EA())); break;                    /* lwc2 */
    case 0x3A: rt_sw(c, EA(), gte_mfc2(c, rt)); break;                    /* swc2 */
    default: rt_fatal(c, "interpreter: illegal or unsupported word %08x at %08x", w, pc);
    }
    r[0] = 0;
    #undef EA
}

static int is_branch(uint32_t w) {
    unsigned op = w >> 26, fn = w & 63;
    if (op == 0) return fn == 0x08 || fn == 0x09;
    return op == 1 || op == 2 || op == 3 || (op >= 4 && op <= 7) || (op == 0x12 && ((w >> 21) & 31) == 8);       /* bc2f / bc2t */
}

static void commit(Cpu *c, Pending *pl, int wrote) { if (pl->reg > 0 && pl->reg != wrote) c->r[pl->reg] = pl->val; pl->reg = -1; }

/* Runs guest code at `target` until it returns to the address that was in $ra on entry. */
void rt_interp_call(Cpu *c, uint32_t target) {
    static int log = -1; if (log < 0) log = getenv("PS1_INTERPLOG") != NULL;
    uint32_t home = c->r[31], pc = target;
    if (log) fprintf(stderr, "[interp] enter %08x (ra %08x, depth %u)\n", target, home, g_depth);
    if (++g_depth > 1000) rt_fatal(c, "interpreter: nesting too deep at %08x", target);
    Pending pl = {-1, 0, -1};
    int jumped = 0;                                          /* the previous step changed the pc non-sequentially: look the new pc up in the function table */
    for (;;) {
#ifdef RT_INTERP_TEST
        if (rt_interp_test_out(pc) || c->exc) break;
#endif
        if (jumped) {
            jumped = 0;
            if (pc == home) break;                           /* returned to the caller */
            uint32_t pk = pc & 0x1FFFFFFFu;
            if (rt_fn_name(pc) || pk == 0xA0 || pk == 0xB0 || pk == 0xC0) {   /* generated function, hook or BIOS entry: call it, then continue where $ra says */
                if (pl.reg >= 0) commit(c, &pl, -1);
                uint32_t ret = c->r[31];
                rt_call(c, pc);
                pc = ret; jumped = 1;
                continue;
            }
        }
        uint32_t w = fetch(c, pc);
        g_interp_insns++;
        g_ring[g_ringi++ & 1023] = pc;
#ifdef RT_CALLTRACE
        rt_pclog(c, pc);                                   /* PS1_PCLOG also covers interpreted code */
#endif
        Pending nl;
        if (!is_branch(w)) { exec_plain(c, pc, w, &pl, &nl); commit(c, &pl, nl.wrote); pl = nl; pc += 4; continue; }
        /* branch: decide, link, run the delay slot, then jump */
        unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63;
        int32_t simm = (int16_t)(uint16_t)w;
        uint32_t *r = c->r, tgt = 0, link = pc + 8;
        int taken = 1, countable = 0, linkreg = 0;
        switch (op) {
        case 0x00: tgt = r[rs]; if (fn == 0x09) linkreg = (int)((w >> 11) & 31); break;
        case 0x01: { int32_t v = (int32_t)r[rs]; int ge = (rt & 1); taken = ge ? v >= 0 : v < 0; tgt = pc + 4 + ((uint32_t)simm << 2); countable = 1; if ((rt & 0x1E) == 0x10) linkreg = 31; break; }
        case 0x02: tgt = ((pc + 4) & 0xF0000000u) | ((w & 0x3FFFFFF) << 2); countable = 1; break;
        case 0x03: tgt = ((pc + 4) & 0xF0000000u) | ((w & 0x3FFFFFF) << 2); linkreg = 31; break;
        case 0x04: taken = r[rs] == r[rt]; tgt = pc + 4 + ((uint32_t)simm << 2); countable = 1; break;
        case 0x05: taken = r[rs] != r[rt]; tgt = pc + 4 + ((uint32_t)simm << 2); countable = 1; break;
        case 0x06: taken = (int32_t)r[rs] <= 0; tgt = pc + 4 + ((uint32_t)simm << 2); countable = 1; break;
        case 0x12: taken = !(rt & 1); tgt = pc + 4 + ((uint32_t)simm << 2); countable = 1; break;      /* the COP2 condition flag reads false (psx-spx): bc2f always branches, bc2t never */
        default:   taken = (int32_t)r[rs] > 0; tgt = pc + 4 + ((uint32_t)simm << 2); countable = 1; break;
        }
        commit(c, &pl, linkreg ? linkreg : -1);                                         /* the load pending before the branch is visible to the slot */
        if (linkreg) c->r[linkreg] = link;
        uint32_t sw = fetch(c, pc + 4);
        if (is_branch(sw)) rt_fatal(c, "interpreter: branch in a delay slot at %08x", pc + 4);
        g_interp_insns++;
        exec_plain(c, pc + 4, sw, &pl, &nl);
        pl = nl;
        if (taken) {
            if (countable && tgt <= pc && --c->yield <= 0) rt_yield(c);
            pc = tgt; jumped = 1;
        } else pc += 8;
    }
    g_depth--;
    if (pl.reg >= 0) commit(c, &pl, -1);
    c->pc = pc;
}
