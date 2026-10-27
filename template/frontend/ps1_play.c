/* Native player: the whole-game build with a window, sound and input (queue P5-3). SDL2.
 *   ps1_play DISC.bin [--card FILE] [--scale N] [--fullscreen] [--no-sync] [--ticks N] [--record FILE]
 * Keys: arrows = d-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L1/R1, E/R = L2/R2, Enter = start, Right Shift = select, F11 = fullscreen, Esc = quit, hold Tab = fast forward, F12 = screenshot (logs/).
 * Game pads (SDL game controller mapping) work too. Build: python3 -I tools/recomp/build_game.py --frontend */
#include <SDL.h>
#include <setjmp.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "io.h"
#include "rt.h"

uint32_t rt_hle_ticks(void);
#include "../runtime/game_boot.inc"

static jmp_buf g_jb;
static char g_msg[700];
static void on_fatal(const char *m) { snprintf(g_msg, sizeof g_msg, "%s", m); longjmp(g_jb, 1); }

typedef struct {
    SDL_Window *win; SDL_Renderer *ren; SDL_Texture *tex; int tw, th;
    SDL_AudioDeviceID audio; SDL_GameController *pad;
    FILE *rec; uint32_t last_word;
    uint8_t *rgb; int sync; uint64_t t0; unsigned frame; int fullscreen; int quit; int ff;
} Host;

/* Pad word as the BIOS pad call returns it (bit map of runtime/hle.c `rt_pad_script_add`, active low, the 16 bits of pad 1 in the low half): select 8, L3 9, R3 10, start 11,
 * up 12, right 13, down 14, left 15, L2 0, R2 1, L1 2, R1 3, triangle 4, circle 5, cross 6, square 7. */
static uint32_t pad_word(Host *h) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint32_t w = 0xFFFFFFFFu;
    #define B(cond, bit) do { if (cond) w &= ~(1u << (bit)); } while (0)
    B(k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE], 8); B(k[SDL_SCANCODE_RETURN], 11);
    B(k[SDL_SCANCODE_UP], 12); B(k[SDL_SCANCODE_RIGHT], 13); B(k[SDL_SCANCODE_DOWN], 14); B(k[SDL_SCANCODE_LEFT], 15);
    B(k[SDL_SCANCODE_E], 0); B(k[SDL_SCANCODE_R], 1); B(k[SDL_SCANCODE_Q], 2); B(k[SDL_SCANCODE_W], 3);
    B(k[SDL_SCANCODE_S], 4); B(k[SDL_SCANCODE_X], 5); B(k[SDL_SCANCODE_Z], 6); B(k[SDL_SCANCODE_A], 7);
    if (h->pad) {
        SDL_GameController *g = h->pad;
        B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_BACK), 8); B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_START), 11);
        B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_UP), 12); B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_RIGHT), 13);
        B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_DOWN), 14); B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_LEFT), 15);
        B(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000, 0); B(SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000, 1);
        B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_LEFTSHOULDER), 2); B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), 3);
        B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_Y), 4); B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_B), 5);
        B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_A), 6); B(SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_X), 7);
        int lx = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
        B(ly < -16000, 12); B(lx > 16000, 13); B(ly > 16000, 14); B(lx < -16000, 15);
    }
    #undef B
    return w;
}

static void present(Host *h, Cpu *c) {
    int w, hh;
    int on = io_present_rgb(rt_io_ctx(c), h->rgb, 1024, 512, &w, &hh);
    (void)on;
    if (w != h->tw || hh != h->th) {
        if (h->tex) SDL_DestroyTexture(h->tex);
        h->tex = SDL_CreateTexture(h->ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, hh);
        h->tw = w; h->th = hh;
    }
    SDL_UpdateTexture(h->tex, NULL, h->rgb, w * 3);
    SDL_RenderClear(h->ren);
    SDL_RenderCopy(h->ren, h->tex, NULL, NULL);                    /* the window keeps a 4:3 logical size: SDL scales the texture */
    SDL_RenderPresent(h->ren);
}

static double now_s(const Host *h) { return (double)(SDL_GetPerformanceCounter() - h->t0) / (double)SDL_GetPerformanceFrequency(); }

/* Called once per virtual frame, after the emulation of that frame. Order matters for smoothness (finding 0044): first wait for the wall-clock moment of this frame, then present, then
 * read the input for the NEXT frame. Presenting right after the computation (and sleeping afterwards) made the interval between pictures follow the emulation cost of each frame
 * (4..31 ms measured instead of 16.7). */
static int g_scripted;
static void tick_hook(Cpu *c, void *ud) {
    Host *h = ud;
    h->frame++;
    int ff = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB];                       /* fast forward while Tab is held: no wall-clock wait, one picture in 4, no audio */
    if (ff != h->ff) { h->ff = ff; fprintf(stderr, "[play] fast forward %s at tick %u\n", ff ? "on" : "off", rt_hle_ticks()); if (!ff) h->t0 = SDL_GetPerformanceCounter() - (Uint64)((double)h->frame * (1001.0 / 60000.0) * (double)SDL_GetPerformanceFrequency()); }
    if (h->sync && !ff) {
        double target = (double)h->frame * (1001.0 / 60000.0);                    /* NTSC 59.94 Hz */
        for (;;) {
            double d = target - now_s(h);
            if (d <= 0) break;
            if (d > 0.002) SDL_Delay(1); else SDL_Delay(0);
        }
        if (now_s(h) > target + 0.5) h->t0 = SDL_GetPerformanceCounter() - (Uint64)(target * (double)SDL_GetPerformanceFrequency());   /* fell far behind: do not try to catch up */
    }
    if (!ff || (h->frame & 3) == 0) present(h, c);
    { static unsigned shot_t = ~0u; static const char *shot_f; static int si;                  /* debug aid: PS1_SHOT=TICK:FILE.ppm writes the picture of that virtual frame (headless check of what the player shows: SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy) */
      if (!si) { si = 1; const char *e = getenv("PS1_SHOT"); if (e) { shot_t = (unsigned)atoi(e); const char *k = strchr(e, ':'); shot_f = k ? k + 1 : "shot.ppm"; } }
      if (rt_hle_ticks() == shot_t && h->tw) { FILE *f = fopen(shot_f, "wb"); if (f) { fprintf(f, "P6\n%d %d\n255\n", h->tw, h->th); fwrite(h->rgb, 3, (size_t)h->tw * h->th, f); fclose(f); fprintf(stderr, "[play] %s\n", shot_f); } } }
    { static int pl = -1; static uint64_t last; static double mn = 1e9, mx = 0, sum = 0; static unsigned n;           /* debug aid: PS1_PACELOG=1 prints the wall-clock interval between pictures (ms) every 600 ticks */
      if (pl < 0) pl = getenv("PS1_PACELOG") != NULL;
      if (pl) { uint64_t t = SDL_GetPerformanceCounter(); if (last) { double ms = 1000.0 * (double)(t - last) / (double)SDL_GetPerformanceFrequency(); if (ms < mn) mn = ms; if (ms > mx) mx = ms; sum += ms; n++; if (n == 600) { fprintf(stderr, "[pace] 600 pictures: mean %.2f ms min %.2f max %.2f\n", sum / n, mn, mx); n = 0; sum = 0; mn = 1e9; mx = 0; } } last = t; } }
    /* audio: the SPU model produced 735 frames this tick */
    int16_t buf[2 * 1024];
    int n;
    static double sq; static unsigned sn;                                                                       /* level of what the SPU produced (log line every 600 ticks) */
    while ((n = io_spu_pull(rt_io_ctx(c), buf, 1024)) > 0) { for (int i = 0; i < 2 * n; i++) sq += (double)buf[i] * buf[i]; sn += 2 * (unsigned)n; if (!ff) SDL_QueueAudio(h->audio, buf, (Uint32)n * 4); }
    if (h->frame % 600 == 0) { fprintf(stderr, "[play] tick %u audio rms %d queued %u bytes device %s\n", rt_hle_ticks(), sn ? (int)sqrt(sq / sn) : 0, h->audio ? SDL_GetQueuedAudioSize(h->audio) : 0, h->audio ? "open" : "NOT OPEN"); sq = 0; sn = 0; }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) h->quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) { h->fullscreen = !h->fullscreen; SDL_SetWindowFullscreen(h->win, h->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12 && h->tw) {                      /* screenshot of the last picture: logs/shot-TICK.ppm */
            char fn[64]; snprintf(fn, sizeof fn, "logs/shot-%u.ppm", rt_hle_ticks()); FILE *f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", h->tw, h->th); fwrite(h->rgb, 3, (size_t)h->tw * h->th, f); fclose(f); fprintf(stderr, "[play] %s\n", fn); }
        }
        else if (e.type == SDL_CONTROLLERDEVICEADDED && !h->pad) h->pad = SDL_GameControllerOpen(e.cdevice.which);
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED && h->pad) { SDL_GameControllerClose(h->pad); h->pad = NULL; }
    }
    if (h->quit) { rt_hle_card_save(); SDL_Quit(); exit(0); }
    { uint32_t w = pad_word(h); if (!g_scripted) rt_pad_set(w);                                    /* the input of the next frame is sampled as late as possible */
      if (h->rec && w != h->last_word) { fprintf(h->rec, "%u %08x\n", rt_hle_ticks() + 1, w); fflush(h->rec); h->last_word = w; } }       /* recorded for the NEXT frame (replay applies it from that tick on) */
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ps1_play DISC.bin [--card FILE] [--scale N] [--fullscreen] [--no-sync] [--ticks N]\n"); return 2; }
    const char *card = GAME_CARD_FILE;
    int scale = 2, fullscreen = 0, sync = 1; unsigned ticks = 0; FILE *rec = NULL; int scripted = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--no-sync")) sync = 0;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec = fopen(argv[++i], "w");        /* every change of the pad word with its virtual frame: replay with run_game --replay */
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) { rt_pad_script_add(argv[++i]); scripted = 1; }       /* scripted input (as run_game): the keyboard is then ignored */
        else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = (unsigned)atoi(argv[++i]);
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    Host h; memset(&h, 0, sizeof h);
    h.sync = sync; g_scripted = scripted; h.fullscreen = fullscreen; h.rec = rec; h.last_word = 0xFFFFFFFFu;
    h.rgb = malloc(1024 * 512 * 3);
    h.win = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale, 240 * scale, SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    h.ren = SDL_CreateRenderer(h.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!h.ren) h.ren = SDL_CreateRenderer(h.win, -1, 0);
    SDL_RenderSetLogicalSize(h.ren, 320, 240);
    SDL_AudioSpec want, have; SDL_zero(want);
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    h.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (h.audio) SDL_PauseAudioDevice(h.audio, 0); else fprintf(stderr, "[play] audio device could not be opened: %s\n", SDL_GetError());
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { h.pad = SDL_GameControllerOpen(i); break; }
    h.t0 = SDL_GetPerformanceCounter();
    Cpu cpu, *c = &cpu;
    uint32_t entry;
    if (game_boot(c, argv[1], card, NULL, &entry)) { fprintf(stderr, "cannot open disc image\n"); return 2; }
    rt_set_tick_hook(tick_hook, &h);
    rt_set_fatal_handler(on_fatal);
    if (ticks) rt_set_max_ticks(ticks);
    if (setjmp(g_jb)) { fprintf(stderr, "%s\n", g_msg); rt_hle_card_save(); SDL_Quit(); return 1; }
    rt_run_entry(c, entry);
    rt_hle_card_save();
    SDL_Quit();
    return 0;
}
