#include <stdio.h>
#include <stdlib.h>
#include "rt.h"
__attribute__((weak)) uint32_t rt_hle_ticks(void) { return 0; }      /* hle.c provides the real tick counter; weak so tests without the BIOS layer link */
static FILE *g_t;
static uint32_t g_watch; static int g_watch_init;   /* debug aid: env PS1_WATCH=hexaddr prints every CPU store that overlaps that word */
void rt_trace_open(const char *path) { g_t = fopen(path, "w"); }
void rt_trace_close(void) { if (g_t) { fclose(g_t); g_t = NULL; } }
void rt_trace_store(const Cpu *c, uint32_t a, unsigned size, uint32_t v) {
    if (!g_watch_init) { const char *e = getenv("PS1_WATCH"); g_watch = e ? (uint32_t)strtoul(e, NULL, 16) : 0; g_watch_init = 1; }
    if (g_watch && a < g_watch + 4 && a + size > g_watch) fprintf(stderr, "[watch] tick %u store %u bytes at %08x = %08x pc=%08x ra=%08x\n", rt_hle_ticks(), size, a, v, c->pc, c->r[31]);
    if (g_t) fprintf(g_t, "st %08x %08x %u %08x\n", c->pc, a, size, v);
}
void rt_trace_hle(const Cpu *c, const char *name) {
    if (g_t) fprintf(g_t, "hle %s %08x %08x %08x %08x %08x %08x\n", name, c->r[4], c->r[5], c->r[6], c->r[7], c->r[29], c->r[31]);
}
