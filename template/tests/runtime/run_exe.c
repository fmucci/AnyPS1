/* Harness for the whole-executable differential tests (queue P2-3/P2-4): loads a PS-X EXE into the runtime, runs the recompiled entry until
 * the program returns, calls DoExecute or hits something unproven (fatal), and prints a summary that tests/recomp/test_exe_trace.py compares
 * with the oracle. Compile with -DGEN_REGISTER=gen_register_x -DGEN_HOOKS=gen_hooks_x. usage: run_exe DISC.bin EXE TRACE [--low LO-HI]... */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "io.h"

void GEN_REGISTER(void);
void GEN_HOOKS(void);
void GEN_CODE(void);

static jmp_buf g_jb;
static char g_msg[700];
static void on_fatal(const char *m) { snprintf(g_msg, sizeof g_msg, "%s", m); longjmp(g_jb, 1); }

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: run_exe DISC.bin EXE TRACE [--low LO-HI]...\n"); return 2; }
    Cpu cpu, *c = &cpu;
    rt_init(c);
    for (int i = 4; i + 1 < argc; i++) if (strcmp(argv[i], "--low") == 0) {            /* --low LO-HI (hex): a kernel-area range this program is known to read (zeros) */
        unsigned lo, hi; if (sscanf(argv[++i], "%x-%x", &lo, &hi) == 2) rt_allow_low_ram(c, lo, hi, "range given with --low (see the game's stubs table for the evidence)");
    }
    if (getenv("PS1_DEFAULT_SP")) rt_allow_low_ram(c, 0x0, 0x10000, "public ps1-tests (PSn00bSDK start-up) copy the BIOS version string from RAM 0x180: the BIOS is not present, modelled as zeros (empty string)");
    rt_trace_open(argv[3]);
    if (!rt_hle_init(argv[1])) { fprintf(stderr, "cannot open disc image\n"); return 2; }
    rt_io_attach_hw(c);
    if (getenv("PS1_CARD")) rt_hle_set_card(getenv("PS1_CARD"));
    GEN_HOOKS();
    GEN_CODE();
    GEN_REGISTER();
    RtExeInfo e;
    rt_exe_load(c, argv[2], &e);
    c->r[28] = e.gp;
    if (e.sp_base) c->r[29] = c->r[30] = e.sp_base + e.sp_off;
    else if (getenv("PS1_DEFAULT_SP")) c->r[29] = c->r[30] = 0x801FFF00u;       /* programs whose header has SP 0 (the public ps1-tests): the loader gives them a stack at the top of RAM */
    if (getenv("PS1_HANDOFF_HEX")) { const char *hx = getenv("PS1_HANDOFF_HEX"); uint8_t hb[0x100]; for (unsigned k = 0; k < 0x100 && hx[2 * k]; k++) { unsigned v; sscanf(hx + 2 * k, "%2x", &v); hb[k] = (uint8_t)v; } rt_mem_write(c, 0x80010000u, hb, sizeof hb); }
    c->r[31] = 0xFFFFFFF0u;   /* the oracle's SENTINEL */
    rt_set_fatal_handler(on_fatal);
    if (getenv("PS1_MAX_TICKS")) rt_set_max_ticks((unsigned)atoi(getenv("PS1_MAX_TICKS")));
    int stopped;
    g_msg[0] = 0;
    if (setjmp(g_jb)) stopped = -1;
    else stopped = rt_run_entry(c, e.pc);
    rt_trace_close();
    size_t n; const char *out = rt_hle_stdout(&n);
    uint32_t ex[3]; rt_hle_last_exec(ex);
    uint64_t h = 0xcbf29ce484222325ull;
    static uint8_t buf[0x200000];
    rt_mem_read(c, 0x80010000, buf, 0x1F0000);
    h = fnv(h, buf, 0x1F0000);
    if (low) { rt_mem_read(c, 0x80009000, buf, 0x1000); h = fnv(h, buf, 0x1000); }
    if (getenv("PS1_PPM")) io_dump_ppm(rt_io_ctx(c), getenv("PS1_PPM"), 0, 0, 1024, 512);
    printf("hw %016llx\n", (unsigned long long)io_state_hash(rt_io_ctx(c)));
    if (getenv("PS1_CARD")) rt_hle_card_save();
    printf("stopped %d\nexec %08x %08x %08x\nramhash %016llx\nstdout %zu\n", stopped, ex[0], ex[1], ex[2], (unsigned long long)h, n);
    fwrite(out, 1, n, stdout);
    printf("\nmsg %s\n", g_msg);
    return 0;
}
