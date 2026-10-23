/* Guest function table and indirect-call dispatch (queue P2-8, decisions/0002 section 3.2). */
#include <stdlib.h>
#include "rt.h"

typedef struct { uint32_t key; RtFn fn; const char *name; RtFnKind kind; uint32_t param, param2; } Ent;
static Ent *g_tab;
static size_t g_n, g_cap;

static uint32_t keyof(uint32_t a) { return a & 0x1FFFFFFFu; }   /* KUSEG/KSEG0/KSEG1 mirror each other (psx-spx Memory Map) */

static size_t lower_bound(uint32_t key) {
    size_t lo = 0, hi = g_n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_tab[m].key < key) lo = m + 1; else hi = m; }
    return lo;
}

void rt_fn_register(uint32_t addr, RtFn fn, const char *name, RtFnKind kind) { rt_fn_register_p(addr, fn, name, kind, 0, 0); }

void rt_fn_register_p(uint32_t addr, RtFn fn, const char *name, RtFnKind kind, uint32_t param, uint32_t param2) {
    if ((addr & 3) || !fn || !name) rt_fatal(NULL, "rt_fn_register: bad entry addr=%08x", addr);
    uint32_t k = keyof(addr);
    size_t i = lower_bound(k);
    if (i < g_n && g_tab[i].key == k) rt_fatal(NULL, "rt_fn_register: %08x (%s) already registered as %s", addr, name, g_tab[i].name);
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_tab = realloc(g_tab, g_cap * sizeof *g_tab);
        if (!g_tab) rt_fatal(NULL, "out of memory in function table");
    }
    for (size_t j = g_n; j > i; j--) g_tab[j] = g_tab[j - 1];
    g_tab[i] = (Ent){k, fn, name, kind, param, param2};
    g_n++;
}

const char *rt_fn_name(uint32_t addr) {
    uint32_t k = keyof(addr);
    size_t i = lower_bound(k);
    return (i < g_n && g_tab[i].key == k) ? g_tab[i].name : NULL;
}
size_t rt_fn_count(void) { return g_n; }
typedef struct { const char *name; uint32_t base, size; void (*code)(void), (*hooks)(void), (*reg)(void); int (*check)(Cpu *); int active; } Overlay;
static Overlay g_ov[32];
static unsigned g_nov;
static int overlay_store(Cpu *c, uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    for (unsigned i = 0; i < g_nov; i++) if (g_ov[i].active && p >= g_ov[i].base && p < g_ov[i].base + g_ov[i].size) {
        rt_code_clear_range(g_ov[i].base, g_ov[i].base + g_ov[i].size);
        rt_fn_remove_range(g_ov[i].base, g_ov[i].base + g_ov[i].size);
        g_ov[i].active = 0;
        return 1;
    }
    (void)c;
    return 0;
}
void rt_overlay_add(const char *name, uint32_t base, uint32_t size, void (*code)(void), void (*hooks)(void), void (*reg)(void), int (*check)(Cpu *)) {
    if (g_nov == 32) rt_fatal(NULL, "rt_overlay_add: table full");
    rt_set_code_store_hook(overlay_store);
    g_ov[g_nov++] = (Overlay){name, base & 0x1FFFFFFFu, size, code, hooks, reg, check, 0};
}
static int overlay_try(Cpu *c, uint32_t target) {
    uint32_t p = target & 0x1FFFFFFFu;
    int candidates = 0;
    for (unsigned i = 0; i < g_nov; i++) if (!g_ov[i].active && p >= g_ov[i].base && p < g_ov[i].base + g_ov[i].size) {
        candidates++;
        if (!g_ov[i].check(c)) continue;                                   /* several generated bases can cover the same address; only the one whose relocated code is in RAM is used */
        g_ov[i].code(); g_ov[i].hooks(); g_ov[i].reg(); g_ov[i].active = 1;
        return 1;
    }
    if (candidates) rt_fatal(c, "call into an overlay region (%08x) but the code in RAM matches none of the %d generated bases: find the loader's base with tools/recomp/find_overlay_bases.py and add it to tools/recomp/overlays.csv", target, candidates);
    return 0;
}
void rt_fn_reset_above(uint32_t phys) { for (unsigned i = 0; i < g_nov; i++) if (g_ov[i].base >= phys) g_ov[i].active = 0;  size_t k = 0; while (k < g_n && g_tab[k].key < phys) k++; g_n = k; }
void rt_fn_remove_range(uint32_t lo, uint32_t hi) {
    uint32_t a = lo & 0x1FFFFFFFu, b = hi & 0x1FFFFFFFu; size_t k = 0;
    for (size_t i = 0; i < g_n; i++) if (!(g_tab[i].key >= a && g_tab[i].key < b)) g_tab[k++] = g_tab[i];
    g_n = k;
}
void rt_fn_reset(void) { g_n = 0; }

static RtBiosFn g_bios;
void rt_set_bios_handler(RtBiosFn fn) { g_bios = fn; }

void rt_call(Cpu *c, uint32_t target) {
    if (target & 3) rt_fatal(c, "indirect call to unaligned address %08x (hardware would raise AdEL)", target);
    uint32_t k = keyof(target);
    if ((k == 0xA0 || k == 0xB0 || k == 0xC0) && g_bios) { g_bios(c, (char)('A' + ((k - 0xA0) >> 4)), c->r[9]); return; }
    size_t i = lower_bound(k);
    if (i >= g_n || g_tab[i].key != k) {
        if (overlay_try(c, target)) { i = lower_bound(k); }
        if (i >= g_n || g_tab[i].key != k) {
            static int off = -1; if (off < 0) off = getenv("PS1_NO_INTERP") != NULL;
            if (!off && k < 0x200000u) { rt_interp_call(c, target); return; }          /* no generated function: run the guest code in the interpreter tier (finding 0004) */
            rt_fatal(c, "indirect call to %08x: no generated function or HLE hook is registered there", target);
        }
    }
    c->hook_param = g_tab[i].param;
    c->hook_param2 = g_tab[i].param2;
    g_tab[i].fn(c);
}
