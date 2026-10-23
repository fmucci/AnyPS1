/* Shared boot sequence of the whole-game build (included by tests/runtime/run_game.c and frontend/ps1_play.c): links the generated modules, loads the executable from the disc
 * and prepares the registers. Needs build/game/game_config.h (written by tools/recomp/build_game.py from game/game.json: modules to link, boot file names, extra low-RAM ranges). */
#include <string.h>
#include "io.h"
#include "rt.h"
#include "game_config.h"
void game_overlays(void);

/* returns 0 on success; `entry` receives the executable's entry pc */
static int game_boot(Cpu *c, const char *disc, const char *card, const char *trace, uint32_t *entry) {
    rt_init(c);
    rt_allow_low_ram(c, 0x0, 0x1000, "kernel exception-vector area (zeros: programs read bytes there through small integers used as pointers) and, from 0x800, the exception stack of event callbacks (docs 04, 05)");
    rt_allow_low_ram(c, 0x6EE0, 0x7200, "the kernel device control blocks (10 entries of 80 bytes): an SDK library walks them through [0x150]/[0x154] to find the \"bu\" device before it calls the file BIOS (docs 11); only the first entry is filled, as in the reference BIOS");
    rt_allow_low_ram(c, 0xEFF0, 0xEFF8, "the name string of that first device control block (\"bu\")");
    { uint8_t *kr = rt_ram_ptr(c); uint32_t v;
      v = 0x6EE0; memcpy(kr + 0x150, &v, 4); v = 0x320; memcpy(kr + 0x154, &v, 4);          /* [0x150] = table, [0x154] = its size in bytes */
      v = 0xEFF0; memcpy(kr + 0x6EE0, &v, 4); memcpy(kr + 0xEFF0, "bu", 3); }
    game_low_ram(c);                                                                      /* the title's own evidence-backed ranges (game/game.json "low_ram") */
    if (getenv("PS1_LOWALL")) rt_allow_low_ram(c, 0x1000, 0x10000, "experiment: the whole kernel area as zeros (PS1_LOWALL)");
#ifdef RT_CALLTRACE
    if (getenv("PS1_BREAK")) rt_break_pc = (uint32_t)strtoul(getenv("PS1_BREAK"), NULL, 16);
#endif
    if (trace) rt_trace_open(trace);
    if (!rt_hle_init(disc)) return 1;
    rt_io_attach_hw(c);
    if (card) rt_hle_set_card(card);
    rt_hle_set_chain(1);
    game_overlays();
    game_link_modules();
    RtExeInfo e;
    if (getenv("PS1_EXE")) rt_exe_load(c, getenv("PS1_EXE"), &e);                       /* debug aid: an extracted executable file instead of the one on the disc */
    else {
        size_t n = 0; uint8_t *img = NULL;
        for (unsigned bi = 0; bi < GAME_N_BOOT && !img; bi++) img = rt_hle_disc_file(GAME_BOOT_NAMES[bi], &n);          /* the executable is read from the disc image itself */
        if (!img) { fprintf(stderr, "the disc image has none of the boot executables listed in game/game.json\n"); return 2; }
        rt_exe_load_mem(c, img, n, GAME_MAIN_EXE, &e);
        free(img);
    }
    c->r[28] = e.gp;
    if (e.sp_base) c->r[29] = c->r[30] = e.sp_base + e.sp_off;
    else c->r[29] = c->r[30] = 0x801FFF00u;                                              /* header SP 0: the BIOS loader uses the STACK value of SYSTEM.CNF; 801FFF00h is what the titles seen so far name (docs 03) */
    c->r[31] = 0xFFFFFFF0u;
    *entry = e.pc;
    return 0;
}
