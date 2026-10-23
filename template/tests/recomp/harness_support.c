/* Test-only implementation of the runtime API declared in runtime/cpu.h: a 256-byte memory window at 0x2000, raw COP0/COP2 arrays.
 * Any out-of-window or misaligned access aborts the test process (the generators never produce them). */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

#define MEM_BASE 0x2000u
#define MEM_SIZE 256u
static uint8_t g_mem[MEM_SIZE];

static uint8_t *at(uint32_t a, uint32_t size) {
    if ((a & (size - 1)) || a < MEM_BASE || a + size > MEM_BASE + MEM_SIZE) {
        fprintf(stderr, "BAD ACCESS %08x size %u\n", a, size);
        abort();
    }
    return &g_mem[a - MEM_BASE];
}
uint32_t rt_lw(Cpu *c, uint32_t a) { (void)c; uint8_t *p = at(a, 4); return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint32_t rt_lh(Cpu *c, uint32_t a) { (void)c; uint8_t *p = at(a, 2); uint32_t v = p[0] | (p[1] << 8); return (v & 0x8000u) ? v | 0xFFFF0000u : v; }
uint32_t rt_lhu(Cpu *c, uint32_t a) { (void)c; uint8_t *p = at(a, 2); return p[0] | (p[1] << 8); }
uint32_t rt_lb(Cpu *c, uint32_t a) { (void)c; uint8_t v = *at(a, 1); return (v & 0x80u) ? v | 0xFFFFFF00u : v; }
uint32_t rt_lbu(Cpu *c, uint32_t a) { (void)c; return *at(a, 1); }
void rt_sw(Cpu *c, uint32_t a, uint32_t v) { (void)c; uint8_t *p = at(a, 4); p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void rt_sh(Cpu *c, uint32_t a, uint32_t v) { (void)c; uint8_t *p = at(a, 2); p[0] = v; p[1] = v >> 8; }
void rt_sb(Cpu *c, uint32_t a, uint32_t v) { (void)c; *at(a, 1) = (uint8_t)v; }
void rt_exception(Cpu *c, uint32_t pc, uint32_t code, uint32_t arg) { c->exc = code; c->exc_pc = pc; c->exc_arg = arg; }
uint32_t cop0_mfc0(Cpu *c, unsigned n) { return c->cop0[n]; }
void cop0_mtc0(Cpu *c, unsigned n, uint32_t v) { c->cop0[n] = v; }
void cop0_rfe(Cpu *c) { c->cop0[12] = (c->cop0[12] & ~0xFu) | ((c->cop0[12] >> 2) & 0xFu); }
uint32_t gte_mfc2(Cpu *c, unsigned n) { return c->gte_d[n]; }
uint32_t gte_cfc2(Cpu *c, unsigned n) { return c->gte_c[n]; }
void gte_mtc2(Cpu *c, unsigned n, uint32_t v) { c->gte_d[n] = v; }
void gte_ctc2(Cpu *c, unsigned n, uint32_t v) { c->gte_c[n] = v; }
void gte_cmd(Cpu *c, uint32_t imm25) { c->gte_ncmds++; c->gte_last_cmd = imm25; }

void test_mem_set(const uint8_t *src) { for (unsigned i = 0; i < MEM_SIZE; i++) g_mem[i] = src[i]; }
const uint8_t *test_mem_get(void) { return g_mem; }

int rt_cov_on; void rt_cov(int g, unsigned id) { (void)g; (void)id; }
