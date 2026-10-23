/* Unit tests of the interpreter tier (runtime/interp.c, finding 0004): hand-assembled programs whose results are derived by hand from the MIPS-I rules (load delay slot, branch delay slot,
 * unaligned loads/stores, calls into registered functions). The interpreter is entered through rt_call exactly as generated code does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

void rt_yield(Cpu *c) { c->yield = RT_YIELD_PERIOD; }
void rt_dma_kick(Cpu *c) { (void)c; }
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { ZERO, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7, S0, S1, S2, S3, S4, S5, S6, S7, T8, T9, K0, K1, GP, SP, FP, RA };
static uint32_t I(unsigned op, unsigned rs, unsigned rt, int imm) { return (op << 26) | (rs << 21) | (rt << 16) | ((uint32_t)imm & 0xFFFF); }
static uint32_t R(unsigned fn, unsigned rs, unsigned rt, unsigned rd, unsigned sa) { return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn; }
#define ADDIU(rt, rs, i) I(9, rs, rt, i)
#define ADDU(rd, rs, rt) R(0x21, rs, rt, rd, 0)
#define SUBU(rd, rs, rt) R(0x23, rs, rt, rd, 0)
#define LW(rt, off, rs) I(0x23, rs, rt, off)
#define SW(rt, off, rs) I(0x2B, rs, rt, off)
#define LWL(rt, off, rs) I(0x22, rs, rt, off)
#define LWR(rt, off, rs) I(0x26, rs, rt, off)
#define SWL(rt, off, rs) I(0x2A, rs, rt, off)
#define SWR(rt, off, rs) I(0x2E, rs, rt, off)
#define BNE(rs, rt, off) I(5, rs, rt, off)
#define JR(rs) R(8, rs, 0, 0, 0)
#define JAL(addr) ((3u << 26) | (((addr) >> 2) & 0x3FFFFFF))
#define NOP 0u

static void load(Cpu *c, uint32_t at, const uint32_t *w, unsigned n) { rt_mem_write(c, at, w, n * 4); }
static void fn_marker(Cpu *c) { c->r[V0] = 0xAAu; }

int main(void) {
    Cpu cpu, *c = &cpu;
    rt_init(c);
    const uint32_t P = 0x80100000u;
    /* 1. loop with a branch delay slot: v0 = 1+2+...+10; t0 counts down, the slot instruction (addiu t0) executes on every iteration, taken or not */
    uint32_t p1[] = {ADDIU(V0, ZERO, 0), ADDIU(T0, ZERO, 10),
                     ADDU(V0, V0, T0),                       /* loop: */
                     BNE(T0, ZERO, -2), ADDIU(T0, T0, -1),   /* delay slot runs before the test of the NEXT iteration sees t0 */
                     JR(RA), NOP};
    /* the branch tests t0 BEFORE its slot decrements it: iterations: t0 = 10..0 -> v0 adds 10,9,...,1 then 0 (the slot makes t0 = 9,...,-1): see expected below */
    load(c, P, p1, 7);
    c->r[RA] = 0x80130000u;
    rt_call(c, P);
    /* trace: v0=0,t0=10 | v0=10; bne t0(10)!=0 taken, slot t0=9 | v0=19; t0=9!=0, slot t0=8 | ... v0 adds t0 each pass: 10+9+...+1 = 55, then t0 = 0 at the bne: not taken, slot t0=-1 */
    CHECK(c->r[V0] == 55 && c->r[T0] == 0xFFFFFFFFu, "loop result v0=%u t0=%08x", c->r[V0], c->r[T0]);
    /* 2. load delay slot: the instruction after a load still sees the OLD register value */
    rt_sw(c, 0x80110000u, 0x1234u);
    uint32_t p2[] = {ADDIU(T0, ZERO, 7), LW(T0, 0, A0), ADDU(V0, T0, ZERO), ADDU(V1, T0, ZERO), JR(RA), NOP};
    load(c, P, p2, 6);
    c->r[A0] = 0x80110000u; c->r[RA] = 0x80130000u;
    rt_call(c, P);
    CHECK(c->r[V0] == 7 && c->r[V1] == 0x1234u, "load delay: slot sees %u, later %04x", c->r[V0], c->r[V1]);
    /* 3. a write to the loaded register in the delay slot cancels the load */
    uint32_t p3[] = {LW(T0, 0, A0), ADDIU(T0, ZERO, 99), ADDU(V0, T0, ZERO), JR(RA), NOP};
    load(c, P, p3, 5);
    c->r[A0] = 0x80110000u; c->r[RA] = 0x80130000u;
    rt_call(c, P);
    CHECK(c->r[V0] == 99, "write in the load delay slot wins (v0=%u)", c->r[V0]);
    /* 4. unaligned word load/store with lwl/lwr and swl/swr: memory bytes 11 22 33 44 55 66 77 88 at 0x80110010; word at +1 = 0x55443322 */
    uint8_t bytes[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    rt_mem_write(c, 0x80110010u, bytes, 8);
    uint32_t p4[] = {LWL(V0, 4, A0), LWR(V0, 1, A0), SWL(V0, 11, A0), SWR(V0, 8, A0), JR(RA), NOP};    /* a0 = 0x80110010: lwl at +4 (end), lwr at +1 (start) */
    load(c, P, p4, 6);
    c->r[A0] = 0x80110010u; c->r[RA] = 0x80130000u;
    rt_call(c, P);
    CHECK(c->r[V0] == 0x55443322u, "lwl/lwr unaligned load %08x", c->r[V0]);
    uint8_t out[8]; rt_mem_read(c, 0x80110018u, out, 4);
    CHECK(out[0] == 0x22 && out[1] == 0x33 && out[2] == 0x44 && out[3] == 0x55, "swl/swr unaligned store %02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
    /* 5. call into a registered function from interpreted code, then continue with the delay slot semantics of jal */
    rt_fn_register(0x80120000u, fn_marker, "marker", RT_FN_GENERATED);
    uint32_t p5[] = {ADDIU(S0, RA, 0), JAL(0x80120000u), ADDIU(A1, ZERO, 5), ADDU(V1, V0, A1), ADDIU(RA, S0, 0), JR(RA), NOP};
    load(c, P, p5, 7);
    c->r[RA] = 0x80130000u;
    rt_call(c, P);
    CHECK(c->r[V0] == 0xAA && c->r[V1] == 0xAA + 5, "call through jal into a registered function (v0=%02x v1=%u)", c->r[V0], c->r[V1]);
    printf("interpreter tests: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
