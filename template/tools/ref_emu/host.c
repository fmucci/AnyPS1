/* Minimal headless libretro host (local tool, not part of any repo): loads a core, runs N frames with a scripted pad, dumps frames as PPM.
 * usage: host CORE.dylib GAME.cue --frames N [--press button@first-last]... [--dump frame:file.ppm]... [--pcs hex,hex,...] */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_variable { const char *key; const char *value; };
struct retro_system_av_info { struct { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; } geometry; struct { double fps, sample_rate; } timing; };
typedef void (*log_cb_t)(int level, const char *fmt, ...);
struct retro_log_callback { log_cb_t log; };

static unsigned g_fmt = 0;  /* 0 = 0RGB1555, 1 = XRGB8888, 2 = RGB565 */
static uint8_t *g_frame; static unsigned g_w, g_h; static size_t g_pitch;
static unsigned g_pad;      /* bit per libretro joypad id */
static void logcb(int level, const char *fmt, ...) { if (level < 2) return; va_list ap; va_start(ap, fmt); fprintf(stderr, "[core] "); vfprintf(stderr, fmt, ap); va_end(ap); }
static int env(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = "work/emu/system"; return 1;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = "work/emu/save"; return 1;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(unsigned *)data; return 1;
    case RETRO_ENVIRONMENT_GET_VARIABLE: { struct retro_variable *v = data; v->value = NULL; if (v->key && !strcmp(v->key, "pcsx_rearmed_memcard1") && getenv("REF_CARD1")) { v->value = "libretro"; return 1; } return 0; }       /* REF_CARD1=1: card 1 is libretro-managed (retro_get_memory_data(SAVE_RAM)); without it the core runs with no card */
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(unsigned char *)data = 0; return 1;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = logcb; return 1;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return 1;
    default: return 0;
    }
}
static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    g_w = w; g_h = h; g_pitch = pitch;
    g_frame = realloc(g_frame, pitch * h); memcpy(g_frame, data, pitch * h);
}
static void audio1(int16_t l, int16_t r) { (void)l; (void)r; }
static FILE *g_audio;
static size_t audio(const int16_t *d, size_t n) { if (g_audio) fwrite(d, 4, n, g_audio); return n; }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) { (void)idx; if (port != 0 || dev != 1) return 0; return (g_pad >> id) & 1; }

static void dump(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f || !g_frame) return;
    fprintf(f, "P6\n%u %u\n255\n", g_w, g_h);
    for (unsigned y = 0; y < g_h; y++) for (unsigned x = 0; x < g_w; x++) {
        uint8_t rgb[3];
        if (g_fmt == 1) { uint32_t p = ((uint32_t *)(g_frame + y * g_pitch))[x]; rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p; }
        else if (g_fmt == 2) { uint16_t p = ((uint16_t *)(g_frame + y * g_pitch))[x]; rgb[0] = ((p >> 11) & 31) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3; }
        else { uint16_t p = ((uint16_t *)(g_frame + y * g_pitch))[x]; rgb[0] = ((p >> 10) & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = (p & 31) << 3; }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static int btn(const char *n) {
    static const char *names[] = {"cross", "square", "select", "start", "up", "down", "left", "right", "circle", "triangle", "l1", "r1", "l2", "r2"};
    static const int ids[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    for (unsigned i = 0; i < 14; i++) if (!strcmp(n, names[i])) return ids[i];
    return -1;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    void *h = dlopen(argv[1], RTLD_NOW); if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define SYM(n) void *n##_ = dlsym(h, #n); if (!n##_) { fprintf(stderr, "missing %s\n", #n); return 1; }
    SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_load_game) SYM(retro_run) SYM(retro_get_system_av_info)
    unsigned frames = 600; struct { int b; unsigned lo, hi; } press[4096]; unsigned np = 0; struct { unsigned f; const char *p; } dumps[1024]; unsigned nd = 0;
    struct { unsigned t; uint32_t w; } *rep = NULL; unsigned nrep = 0, irep = 0;       /* --replay FILE: lines "tick hexword" recorded by ps1_play --record (our pad word, active low) */
    struct { unsigned lo, hi; uint32_t a, v; } poke[8]; unsigned npk = 0;
    const char *card_out = NULL, *card_in = NULL;       /* --savecard FILE: card 1 (libretro SAVE_RAM) after the last frame; --loadcard FILE: card 1 before the first frame */
    unsigned start_f = 0; const char *load_p = NULL, *save_p = NULL;       /* --loadstate FILE:FRAME resumes a savestate at that frame number; --savestate FILE writes one after the last frame */
    unsigned ramdump_f = 0xFFFFFFFFu; const char *ramdump_p = NULL;
    uint32_t poll_addr = 0; unsigned have_poll = 0; uint32_t last_poll = 0xdeadbeef;
    unsigned *wlo = dlsym(h, "ps1_wlo"), *whi = dlsym(h, "ps1_whi");
    unsigned *npc = dlsym(h, "ps1_npc"), *pcs = dlsym(h, "ps1_pcs"), *fr = dlsym(h, "ps1_frame");
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames")) frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--press")) { char nm[16]; unsigned lo, hi; if (np < 4096 && sscanf(argv[++i], "%15[a-z0-9]@%u-%u", nm, &lo, &hi) == 3) { press[np].b = btn(nm); press[np].lo = lo; press[np++].hi = hi; } }
        else if (!strcmp(argv[i], "--dump")) { if (nd >= 1024) { fprintf(stderr, "too many --dump\n"); return 2; } dumps[nd].f = (unsigned)atoi(argv[i + 1]); char *c = strchr(argv[i + 1], ':'); dumps[nd++].p = c + 1; i++; }
        else if (!strcmp(argv[i], "--jal")) { unsigned *jl = dlsym(h, "ps1_jlo"), *jh = dlsym(h, "ps1_jhi"); sscanf(argv[++i], "%u:%u", jl, jh); }
        else if (!strcmp(argv[i], "--bios")) { unsigned *bl = dlsym(h, "ps1_blo"), *bh = dlsym(h, "ps1_bhi"); if (bl && bh) sscanf(argv[++i], "%u:%u", bl, bh); }       /* --bios LO:HI logs every A/B BIOS call of those frames */
        else if (!strcmp(argv[i], "--audio")) { g_audio = fopen(argv[++i], "wb"); }
        else if (!strcmp(argv[i], "--watch")) { *wlo = (unsigned)strtoul(argv[++i], NULL, 16); *whi = (unsigned)strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(argv[i], "--replay")) { FILE *rf = fopen(argv[++i], "r"); unsigned t, w, cap = 0; while (rf && fscanf(rf, "%u %x", &t, &w) == 2) { if (nrep == cap) { cap = cap ? cap * 2 : 1024; rep = realloc(rep, cap * sizeof *rep); } rep[nrep].t = t; rep[nrep++].w = w; } if (rf) fclose(rf); }
        else if (!strcmp(argv[i], "--poke")) { unsigned lo, hi; unsigned a, v; if (npk < 8 && sscanf(argv[++i], "%u-%u:%x=%x", &lo, &hi, &a, &v) == 4) { poke[npk].lo = lo; poke[npk].hi = hi; poke[npk].a = a; poke[npk++].v = v; } }   /* word write into guest RAM before each frame in [lo,hi] (e.g. the level index of the hand-off block) */
        else if (!strcmp(argv[i], "--ramdump")) { ramdump_f = (unsigned)atoi(argv[++i]); char *c = strchr(argv[i], ':'); ramdump_p = c ? c + 1 : NULL; }       /* --ramdump FRAME:FILE writes the 2 MiB of system RAM after that frame */
        else if (!strcmp(argv[i], "--loadstate")) { load_p = argv[++i]; char *c = strrchr(argv[i], ':'); if (c) { *c = 0; start_f = (unsigned)atoi(c + 1); } }
        else if (!strcmp(argv[i], "--savecard")) { card_out = argv[++i]; }
        else if (!strcmp(argv[i], "--loadcard")) { card_in = argv[++i]; }
        else if (!strcmp(argv[i], "--savestate")) { save_p = argv[++i]; }
        else if (!strcmp(argv[i], "--poll")) { poll_addr = (uint32_t)strtoul(argv[++i], NULL, 16); have_poll = 1; }
        else if (!strcmp(argv[i], "--pcs")) { char *s = strdup(argv[++i]); for (char *t = strtok(s, ","); t && *npc < 32; t = strtok(NULL, ",")) pcs[(*npc)++] = (unsigned)strtoul(t, NULL, 16); }
    }
    ((void (*)(void *))retro_set_environment_)(env);
    ((void (*)(void *))retro_set_video_refresh_)(video);
    ((void (*)(void *))retro_set_audio_sample_)(audio1);
    ((void (*)(void *))retro_set_audio_sample_batch_)(audio);
    ((void (*)(void *))retro_set_input_poll_)(poll);
    ((void (*)(void *))retro_set_input_state_)(input);
    ((void (*)(void))retro_init_)();
    struct retro_game_info gi = {argv[2], NULL, 0, NULL};
    if (!((int (*)(void *))retro_load_game_)(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
    struct retro_system_av_info av; ((void (*)(void *))retro_get_system_av_info_)(&av);
    fprintf(stderr, "[host] loaded, fps %.2f\n", av.timing.fps);
    size_t (*ser_size)(void) = dlsym(h, "retro_serialize_size"); int (*ser)(void *, size_t) = dlsym(h, "retro_serialize"); int (*unser)(const void *, size_t) = dlsym(h, "retro_unserialize");
    if (load_p) { FILE *lf = fopen(load_p, "rb"); if (!lf) { fprintf(stderr, "no state\n"); return 1; } size_t n = ser_size(); void *b = malloc(n); size_t got = fread(b, 1, n, lf); fclose(lf); if (got != n || !unser(b, n)) { fprintf(stderr, "bad state\n"); return 1; } free(b); }
    if (card_in) { void *(*gm)(unsigned) = dlsym(h, "retro_get_memory_data"); size_t (*gs)(unsigned) = dlsym(h, "retro_get_memory_size"); uint8_t *m = gm ? gm(0) : NULL; FILE *cf = fopen(card_in, "rb"); if (m && cf) { size_t n = gs ? gs(0) : 131072; if (n > 131072) n = 131072; size_t got = fread(m, 1, n, cf); fprintf(stderr, "[host] card in: %zu bytes\n", got); } if (cf) fclose(cf); }
    for (unsigned f = start_f; f < frames; f++) {
        if (fr) *fr = f;
        g_pad = 0; for (unsigned i = 0; i < np; i++) if (f >= press[i].lo && f <= press[i].hi && press[i].b >= 0) g_pad |= 1u << press[i].b;
        if (npk) { void *(*gm)(unsigned) = dlsym(h, "retro_get_memory_data"); uint8_t *ram = gm ? gm(2) : NULL; for (unsigned i = 0; ram && i < npk; i++) if (f >= poke[i].lo && f <= poke[i].hi) memcpy(ram + (poke[i].a & 0x1fffff), &poke[i].v, 4); }
        if (nrep) {                                      /* recorded pad word -> libretro joypad ids (our bit map: select 8, start 11, up 12, right 13, down 14, left 15, l2 0, r2 1, l1 2, r1 3, triangle 4, circle 5, cross 6, square 7) */
            while (irep + 1 < nrep && rep[irep + 1].t <= f) irep++;
            uint32_t w = rep[irep].t <= f ? rep[irep].w : 0xFFFFFFFFu;
            static const struct { int bit, id; } map[] = {{8, 2}, {11, 3}, {12, 4}, {13, 7}, {14, 5}, {15, 6}, {0, 12}, {1, 13}, {2, 10}, {3, 11}, {4, 9}, {5, 8}, {6, 0}, {7, 1}};
            g_pad = 0; for (unsigned k = 0; k < sizeof map / sizeof map[0]; k++) if (!((w >> map[k].bit) & 1)) g_pad |= 1u << map[k].id;
        }
        ((void (*)(void))retro_run_)();
        if (ramdump_p && f == ramdump_f) { void *(*gm)(unsigned) = dlsym(h, "retro_get_memory_data"); uint8_t *ram = gm ? gm(2) : NULL; FILE *rf = ram ? fopen(ramdump_p, "wb") : NULL; if (rf) { fwrite(ram, 1, 2u << 20, rf); fclose(rf); } }
        for (unsigned i = 0; i < nd; i++) if (dumps[i].f == f) dump(dumps[i].p);
        if (have_poll) { void *(*gm)(unsigned) = dlsym(h, "retro_get_memory_data"); uint8_t *ram = gm ? gm(2) : NULL; if (ram) { uint32_t v; memcpy(&v, ram + (poll_addr & 0x1fffff), 4); if (v != last_poll) { fprintf(stderr, "[poll] frame %u %08x = %08x\n", f, poll_addr, v); last_poll = v; } } }
        if (f % 600 == 0) fprintf(stderr, "[host] frame %u\n", f);
    }
    if (card_out) { void *(*gm)(unsigned) = dlsym(h, "retro_get_memory_data"); uint8_t *m = gm ? gm(0) : NULL; FILE *cf = m ? fopen(card_out, "wb") : NULL; if (cf) { fwrite(m, 1, 131072, cf); fclose(cf); } else fprintf(stderr, "card out failed\n"); }
    if (save_p) { size_t n = ser_size(); void *b = malloc(n); FILE *sf = ser(b, n) ? fopen(save_p, "wb") : NULL; if (sf) { fwrite(b, 1, n, sf); fclose(sf); } else fprintf(stderr, "save failed\n"); free(b); }
    return 0;
}
