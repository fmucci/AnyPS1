/* The whole game: the launcher plus every recompiled executable linked together; DoExecute switches modules (queue P4, finding 0025).
 * usage: run_game DISC.bin [--ticks N] [--press button[+button]@first-last]... [--fuzz SEED:FIRST_FRAME] [--replay FILE] [--ppm FILE] [--audio RAW.s16] [--trace FILE] [--card FILE] [--branch TICK:FILE]
 * Build: python3 -I tools/recomp/build_game.py */
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io.h"
#include "rt.h"

#include "../../runtime/game_boot.inc"

uint32_t rt_hle_ticks(void);
void rt_hle_card_set_path(const char *path);
static const char *g_every_prefix; static unsigned g_every_first, g_every_step;
static struct { unsigned tick; uint32_t addr, val; } g_poke[8192]; static unsigned g_npoke;      /* --pokefile FILE: lines `tick hexaddr hexvalue` write a word at the start of that virtual frame (comparison experiments against a reference run) */
static void poke_hook(Cpu *c, void *ud) {
    (void)ud; unsigned t = rt_hle_ticks();
    for (unsigned i = 0; i < g_npoke; i++) if (g_poke[i].tick == t) rt_mem_write(c, g_poke[i].addr, &g_poke[i].val, 4);
}
static void every_hook(Cpu *c, void *ud) {                 /* --ppm-every PREFIX:FIRST:STEP writes PREFIX<tick>.ppm at tick FIRST, FIRST+STEP, ... */
    (void)ud;
    unsigned t = rt_hle_ticks();
    if (!g_every_prefix || t < g_every_first || (t - g_every_first) % g_every_step) return;
    uint8_t *rgb = malloc(1024 * 512 * 3); int w, h; char name[700];
    io_present_rgb(rt_io_ctx(c), rgb, 1024, 512, &w, &h);
    snprintf(name, sizeof name, "%s%u.ppm", g_every_prefix, t);
    FILE *f = fopen(name, "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 3, (size_t)w * h, f); fclose(f); }
    free(rgb);
}
static struct { unsigned tick; const char *file; } g_br[8]; static unsigned g_nbr;       /* --branch may be given several times; a child keeps the later ones, so a run can fork again from any of its children */
static void branch_hook(Cpu *c, void *ud) {                /* --branch TICK:FILE forks the whole run at that tick: one child per line of FILE, `PREFIX[:FIRST:STEP] press press ...` (PREFIX replaces the --ppm-every prefix and names the child's log and card), so a route can be explored from an in-memory checkpoint */
    (void)c; (void)ud;
    unsigned bi; for (bi = 0; bi < g_nbr; bi++) if (g_br[bi].file && g_br[bi].tick == rt_hle_ticks()) break;
    if (bi == g_nbr) return;
    FILE *bf = fopen(g_br[bi].file, "r"); static char line[1 << 16]; int kids = 0;
    g_br[bi].file = NULL;
    while (bf && fgets(line, sizeof line, bf)) {
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); break; }
        if (pid == 0) {                                      /* child: its own picture prefix, log and card; the press script gets the line's presses appended */
            static char pre[700], log[700], cardp[700];
            unsigned f1, s1; char *colon = strchr(tok, ':');        /* PREFIX or PREFIX:FIRST:STEP */
            if (colon && sscanf(colon + 1, "%u:%u", &f1, &s1) == 2 && s1) { *colon = 0; g_every_first = f1; g_every_step = s1; }
            snprintf(pre, sizeof pre, "%s", tok); g_every_prefix = pre;
            snprintf(log, sizeof log, "%slog.txt", tok); if (!freopen(log, "w", stdout)) _exit(3); dup2(fileno(stdout), 2);
            snprintf(cardp, sizeof cardp, "%scard.mcr", tok); rt_hle_card_set_path(cardp);
            static char *pr[4096]; int np = 0;                  /* tokenise first: rt_pad_script_add uses strtok itself */
            while (np < 4096 && (tok = strtok(NULL, " \t\r\n"))) pr[np++] = tok;
            for (int k = 0; k < np; k++) rt_pad_script_add(pr[k]);
            fclose(bf);
            return;
        }
        kids++;
    }
    if (bf) fclose(bf);
    int st; while (kids-- > 0) wait(&st);
    fflush(NULL); _exit(0);                                  /* the parent only waits */
}
static jmp_buf g_jb;
static char g_msg[700];
static void on_fatal(const char *m) { snprintf(g_msg, sizeof g_msg, "%s", m); longjmp(g_jb, 1); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: run_game DISC.bin [options]\n"); return 2; }
    const char *vram = NULL, *wav = NULL, *ppm = NULL, *trace = NULL, *card = NULL;
    unsigned ticks = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--vram") && i + 1 < argc) vram = argv[++i];
        else if (!strcmp(argv[i], "--ppm-every") && i + 1 < argc) { static char pre[600]; char *a = argv[++i]; if (sscanf(a, "%599[^:]:%u:%u", pre, &g_every_first, &g_every_step) == 3 && g_every_step) { g_every_prefix = pre; } }
        else if (!strcmp(argv[i], "--branch") && i + 1 < argc) { static char bf[8][600]; char *a = argv[++i]; unsigned t; char name[560]; if (g_nbr < 8 && sscanf(a, "%u:%559s", &t, name) == 2) { g_br[g_nbr].tick = t; snprintf(bf[g_nbr], sizeof bf[g_nbr], "%s", name); g_br[g_nbr].file = bf[g_nbr]; g_nbr++; } }
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--pokefile") && i + 1 < argc) { FILE *pf = fopen(argv[++i], "r"); unsigned t, a, v; while (pf && g_npoke < 8192 && fscanf(pf, "%u %x %x", &t, &a, &v) == 3) { g_poke[g_npoke].tick = t; g_poke[g_npoke].addr = a; g_poke[g_npoke++].val = v; } if (pf) fclose(pf); }
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) rt_pad_script_add(argv[++i]);
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) rt_pad_replay_load(argv[++i]);
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) { unsigned sd = 1, fr = 0; sscanf(argv[++i], "%u:%u", &sd, &fr); rt_pad_fuzz(sd, fr); }      /* --fuzz SEED:FIRST_FRAME */
    }
    Cpu cpu, *c = &cpu;
    uint32_t entry;
    if (game_boot(c, argv[1], card, trace, &entry)) { fprintf(stderr, "cannot open disc image\n"); return 2; }
    if (wav) io_spu_capture(rt_io_ctx(c), wav);
    if (g_every_prefix || g_nbr) rt_set_tick_hook(every_hook, NULL);
    if (g_npoke) rt_set_tick_hook(poke_hook, NULL);
    if (g_nbr) rt_set_tick_hook(branch_hook, NULL);
    rt_set_fatal_handler(on_fatal);
    if (ticks) rt_set_max_ticks(ticks);
    int stopped;
    if (setjmp(g_jb)) stopped = -1; else stopped = rt_run_entry(c, entry);
    rt_trace_close();
    if (wav) io_spu_capture_close(rt_io_ctx(c));
    if (getenv("PS1_DUMP")) {                               /* debug aid: PS1_DUMP=hexaddr,len prints guest memory as words after the run */
        char spec[512]; snprintf(spec, sizeof spec, "%s", getenv("PS1_DUMP"));
        for (char *tok = strtok(spec, ";"); tok; tok = strtok(NULL, ";")) {          /* several ranges separated by ';' */
            unsigned a = 0, l = 0; sscanf(tok, "%x,%u", &a, &l);
            for (unsigned k = 0; k < l; k += 4) { uint32_t w; rt_mem_read(c, a + k, &w, 4); if (k % 32 == 0) printf("\n[dump] %08x:", a + k); printf(" %08x", w); }
        }
        printf("\n");
    }
    if (getenv("PS1_RAMDUMP")) { FILE *rf = fopen(getenv("PS1_RAMDUMP"), "wb"); if (rf) { uint8_t *rp = rt_ram_ptr(c); fwrite(rp, 1, 2u << 20, rf); fclose(rf); } }     /* debug aid: the whole 2 MB of RAM after the run */
    size_t n; const char *out = rt_hle_stdout(&n);
    fwrite(out, 1, n, stdout);
    if (vram) io_dump_ppm(rt_io_ctx(c), vram, 0, 0, 1024, 512);       /* whole VRAM, 5-bit colours expanded */
    if (ppm) {
        uint8_t *rgb = malloc(1024 * 512 * 3); int w, h;
        io_present_rgb(rt_io_ctx(c), rgb, 1024, 512, &w, &h);
        FILE *f = fopen(ppm, "wb"); fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 3, (size_t)w * h, f); fclose(f);
        free(rgb);
    }
    if (card) rt_hle_card_save();
    { int ok; printf("\nSR %08x I_STAT %08x I_MASK %08x\n", c->cop0[12], io_read(rt_io_ctx(c), 0x1F801070, 4, &ok), io_read(rt_io_ctx(c), 0x1F801074, 4, &ok)); }
    if (getenv("PS1_INTERPRING")) rt_interp_dump((unsigned)atoi(getenv("PS1_INTERPRING")));
    if (getenv("PS1_REGS")) { for (int i = 0; i < 32; i++) printf("r%d=%08x%s", i, c->r[i], i % 8 == 7 ? "\n" : " "); printf("hi=%08x lo=%08x pc=%08x\n", c->hi, c->lo, c->pc); }       /* debug aid: all registers after the run */
    printf("stopped %d\nmsg %s\n", stopped, g_msg);
    return 0;
}
