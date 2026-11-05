/* Native player: the whole-game build with a window, sound and input (queue P5-3). SDL2.
 *   ps1_play DISC.bin [--card FILE] [--scale N] [--fullscreen] [--no-sync] [--ticks N] [--record FILE]
 * Keys (input.cfg, written on first start): WASD/arrows = d-pad, J/K/L/I = square/cross/circle/triangle, Q/E = L1/R1, R/F = L2/R2, Enter = start, Right Shift = select, mouse sideways = turn (F10 = release the mouse), F11 = fullscreen, Esc = quit, hold Tab = fast forward, F12 = screenshot.
 * Game pads (SDL game controller mapping) work too. Build: python3 -I tools/recomp/build_game.py --frontend */
#include <SDL.h>
#include <setjmp.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "io.h"
#include "rt.h"
#include "pad_input.h"

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
    PadCfg cfg; SDL_Scancode kc[PADIN_MAXBIND]; MouseLook ml; double mdx; int grab, want_grab;       /* keyboard layout (input.cfg) and mouse look */
} Host;

/* Pad word as the BIOS pad call returns it (bit map of runtime/hle.c `rt_pad_script_add`, active low, the 16 bits of pad 1 in the low half): select 8, L3 9, R3 10, start 11,
 * up 12, right 13, down 14, left 15, L2 0, R2 1, L1 2, R1 3, triangle 4, circle 5, cross 6, square 7. */
static uint32_t pad_word(Host *h) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint32_t w = 0xFFFFFFFFu;
    #define B(cond, bit) do { if (cond) w &= ~(1u << (bit)); } while (0)
    for (int i = 0; i < h->cfg.n; i++) if (h->cfg.b[i].bit >= 0 && k[h->kc[i]]) B(1, h->cfg.b[i].bit);       /* input.cfg: see runtime/pad_input.h */
    if (h->grab) {                                                                                         /* mouse look: horizontal motion presses left/right, the buttons are bound in input.cfg */
        Uint32 mb = SDL_GetMouseState(NULL, NULL);
        for (int i = 0; i < 3; i++) if (h->cfg.mouse_btn[i] >= 0 && (mb & SDL_BUTTON(i == 0 ? SDL_BUTTON_LEFT : i == 1 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE))) B(1, h->cfg.mouse_btn[i]);
        w &= padin_mouse_turn(&h->ml, h->mdx, h->cfg.sens); h->mdx = 0;
    }
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

static const char INPUT_CFG_DEFAULT[] =
    "# Keyboard and mouse layout. Lines starting with # are comments. Delete this file to get the defaults back.\n"
    "# scheme wasd      W/A/S/D walk (same as the arrow keys), Q/E = L1/R1, R/F = L2/R2, J/K/L/I = square/cross/circle/triangle, Space = cross\n"
    "# scheme classic   the first versions' layout: Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1, E/R = L2/R2\n"
    "scheme wasd\n"
    "# Mouse look: moving the mouse sideways presses left/right (the game's pad is digital, so the turn speed is the game's own).\n"
    "# F10 releases/captures the mouse (use it in menus). Higher sensitivity = less motion for the same turn.\n"
    "mouse on\n"
    "sensitivity 1.0\n"
    "# Mouse buttons -> pad buttons (none, up, down, left, right, cross, circle, square, triangle, l1, r1, l2, r2, l3, r3, start, select)\n"
    "mouse_left square\n"
    "mouse_right circle\n"
    "mouse_middle triangle\n"
    "# Extra keys: key <SDL key name> <button>, for example:  key Left Shift l1      key Tab none   (Tab is fast-forward and cannot be rebound)\n";

static void load_input(Host *h, int write_default) {
    padin_defaults(&h->cfg);
    FILE *f = fopen("input.cfg", "rb");
    if (!f && write_default) { f = fopen("input.cfg", "wb"); if (f) { fwrite(INPUT_CFG_DEFAULT, 1, sizeof INPUT_CFG_DEFAULT - 1, f); fclose(f); fprintf(stderr, "[play] wrote input.cfg (keyboard/mouse layout)\n"); } f = NULL; }
    if (f) {
        char txt[8192]; size_t n = fread(txt, 1, sizeof txt - 1, f); txt[n] = 0; fclose(f);
        char err[160] = ""; int bad = padin_parse(&h->cfg, txt, err, sizeof err);
        if (bad) fprintf(stderr, "[play] input.cfg: %d line(s) not understood, first: %s\n", bad, err);
    }
    int m = 0;
    for (int i = 0; i < h->cfg.n; i++) {
        SDL_Scancode sc = SDL_GetScancodeFromName(h->cfg.b[i].key);
        if (sc == SDL_SCANCODE_UNKNOWN || sc == SDL_SCANCODE_TAB) { fprintf(stderr, "[play] input.cfg: unknown key '%s'\n", h->cfg.b[i].key); continue; }
        h->cfg.b[m] = h->cfg.b[i]; h->kc[m++] = sc;
    }
    h->cfg.n = m;
}

static void set_grab(Host *h, int on) { h->grab = on; SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE); if (!on) h->mdx = 0; }

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

/* Audio queue keeping: the log of a Windows tester showed the queue sitting at 2 frames (~6 KB, p10 4.3 KB) - any frame that takes longer than ~30 ms empties it and pops/crackles.
 * Keep ~5 frames (~15 KB, 83 ms) queued: prime it with silence at the start, refill with silence when it runs low, and drop a chunk when the device clock lags far behind the wall clock. */
#define AUDIO_TARGET_BYTES (5 * 2940)
static void audio_queue(Host *h, const int16_t *buf, int frames) {
    static int primed;
    if (!primed) { primed = 1; static int16_t z[2 * 735]; for (int i = 0; i < 5; i++) SDL_QueueAudio(h->audio, z, sizeof z); }
    Uint32 q = SDL_GetQueuedAudioSize(h->audio);
    if (q > 4 * AUDIO_TARGET_BYTES) return;                                                      /* the device is slower than the emulation clock: drop this chunk instead of growing the delay */
    if (q < AUDIO_TARGET_BYTES / 3) { static int16_t z[2 * 735]; SDL_QueueAudio(h->audio, z, sizeof z); }   /* about to run dry: a short silence is better than a gap in the middle of a sample */
    SDL_QueueAudio(h->audio, buf, (Uint32)frames * 4);
}

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
    while ((n = io_spu_pull(rt_io_ctx(c), buf, 1024)) > 0) { for (int i = 0; i < 2 * n; i++) sq += (double)buf[i] * buf[i]; sn += 2 * (unsigned)n; if (!ff && h->audio) audio_queue(h, buf, n); }
    if (h->frame % 600 == 0) { fprintf(stderr, "[play] tick %u audio rms %d queued %u bytes device %s\n", rt_hle_ticks(), sn ? (int)sqrt(sq / sn) : 0, h->audio ? SDL_GetQueuedAudioSize(h->audio) : 0, h->audio ? "open" : "NOT OPEN"); sq = 0; sn = 0; }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) h->quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) { h->fullscreen = !h->fullscreen; SDL_SetWindowFullscreen(h->win, h->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12 && h->tw) {                      /* screenshot of the last picture: logs/shot-TICK.ppm */
            char fn[64]; snprintf(fn, sizeof fn, "logs/shot-%u.ppm", rt_hle_ticks()); FILE *f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", h->tw, h->th); fwrite(h->rgb, 3, (size_t)h->tw * h->th, f); fclose(f); fprintf(stderr, "[play] %s\n", fn); }
        }
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10 && h->want_grab >= 0) { h->want_grab = !h->want_grab; set_grab(h, h->want_grab); fprintf(stderr, "[play] mouse look %s\n", h->want_grab ? "on" : "off"); }
        else if (e.type == SDL_MOUSEMOTION && h->grab) h->mdx += e.motion.xrel;
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && h->grab) set_grab(h, 0);
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && h->want_grab > 0 && !h->grab) set_grab(h, 1);
        else if (e.type == SDL_CONTROLLERDEVICEADDED && !h->pad) h->pad = SDL_GameControllerOpen(e.cdevice.which);
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED && h->pad) { SDL_GameControllerClose(h->pad); h->pad = NULL; }
    }
    { static unsigned seen, quiet; unsigned g = rt_hle_card_gen();                                 /* the card is written to disk 30 frames after the game's last change to it, so a force-quit, a crash or a power loss keeps the save (it was written only at exit) */
      if (g != seen) { seen = g; quiet = 0; } else if (quiet < 1000 && ++quiet == 30 && !rt_hle_card_save()) fprintf(stderr, "[play] could not write the memory card\n"); }
    if (h->quit) { rt_hle_card_save(); SDL_Quit(); exit(0); }
    { uint32_t w = pad_word(h); if (!g_scripted) rt_pad_set(w);                                    /* the input of the next frame is sampled as late as possible */
      if (h->rec && w != h->last_word) { fprintf(h->rec, "%u %08x\n", rt_hle_ticks() + 1, w); fflush(h->rec); h->last_word = w; } }       /* recorded for the NEXT frame (replay applies it from that tick on) */
}

#ifdef _WIN32
#include <windows.h>
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) { fprintf(stderr, "[play] CRASH: exception %08lx at %p\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress); fflush(stderr); return EXCEPTION_EXECUTE_HANDLER; }
#endif
static void fail_box(const char *what, const char *detail) {          /* the console output goes to logs/session-*.log, so a failure at start-up must be visible some other way */
    fprintf(stderr, "[play] %s: %s\n", what, detail ? detail : "");
    char msg[1400]; snprintf(msg, sizeof msg, "%s:\n%s\n\nDetails are in the logs folder.", what, detail ? detail : "");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_TITLE, msg, NULL);
}

int main(int argc, char **argv) {
    static char dflt[3][64]; char *av[3]; FILE *rec0 = NULL;
    if (argc < 2) {                                                     /* started by double-clicking the .exe (not through play.bat): find the disc next to it, keep the logs ourselves */
#ifdef _WIN32
        SetUnhandledExceptionFilter(crash_filter);                      /* a crash leaves a line in the log instead of a silently closing window */
        char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) { char *sl = strrchr(exe, '\\'); if (sl) { *sl = 0; SetCurrentDirectoryA(exe); } }
        CreateDirectoryA("logs", NULL);
#endif
        time_t t = time(NULL); struct tm *tm = localtime(&t); char fn[96];
        strftime(fn, sizeof fn, "logs/session-%Y%m%d-%H%M%S.log", tm); freopen(fn, "w", stderr);
        strftime(fn, sizeof fn, "logs/session-%Y%m%d-%H%M%S.rec", tm); rec0 = fopen(fn, "w");
        FILE *probe = fopen("disc/disc.map", "r");
        snprintf(dflt[0], sizeof dflt[0], "ps1_play"); snprintf(dflt[1], sizeof dflt[1], "%s", probe ? "disc" : "game.bin"); if (probe) fclose(probe);
        av[0] = dflt[0]; av[1] = dflt[1]; argv = av; argc = 2;
        fprintf(stderr, "[play] started without arguments: disc = %s\n", dflt[1]); fflush(stderr);
    }
    const char *card = GAME_CARD_FILE;
    int scale = 2, fullscreen = 0, sync = 1; unsigned ticks = 0; FILE *rec = rec0; int scripted = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--no-sync")) sync = 0;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec = fopen(argv[++i], "w");        /* every change of the pad word with its virtual frame: replay with run_game --replay */
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) { rt_pad_script_add(argv[++i]); scripted = 1; }       /* scripted input (as run_game): the keyboard is then ignored */
        else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = (unsigned)atoi(argv[++i]);
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {          /* a machine without audio or game controller support (a remote desktop, a VM): retry with video only */
        fprintf(stderr, "SDL (video+audio+controller): %s\n", SDL_GetError());
        if (SDL_Init(SDL_INIT_VIDEO) != 0) { fail_box("SDL could not start", SDL_GetError()); return 1; }
    }
    Host h; memset(&h, 0, sizeof h);
    h.sync = sync; g_scripted = scripted; h.fullscreen = fullscreen; h.rec = rec; h.last_word = 0xFFFFFFFFu;
    h.rgb = malloc(1024 * 512 * 3);
    h.win = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale, 240 * scale, SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!h.win) { fail_box("could not create the window", SDL_GetError()); return 1; }
    h.ren = SDL_CreateRenderer(h.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!h.ren) { fprintf(stderr, "[play] accelerated renderer: %s\n", SDL_GetError()); h.ren = SDL_CreateRenderer(h.win, -1, 0); }
    if (!h.ren) { fprintf(stderr, "[play] default renderer: %s\n", SDL_GetError()); h.ren = SDL_CreateRenderer(h.win, -1, SDL_RENDERER_SOFTWARE); }       /* remote desktop sessions often have no GPU */
    if (!h.ren) { fail_box("could not create a renderer", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(h.ren, 320, 240);
    SDL_AudioSpec want, have; SDL_zero(want);
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 2048;
    h.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (h.audio) SDL_PauseAudioDevice(h.audio, 0); else fprintf(stderr, "[play] audio device could not be opened: %s\n", SDL_GetError());
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { h.pad = SDL_GameControllerOpen(i); break; }
    load_input(&h, !g_scripted);
    h.want_grab = (h.cfg.mouse_on && !g_scripted && !SDL_getenv("SDL_VIDEODRIVER")) ? 1 : -1;               /* -1: no mouse look (scripted run, headless driver or "mouse off") */
    if (h.want_grab > 0) set_grab(&h, 1);
    h.t0 = SDL_GetPerformanceCounter();
    Cpu cpu, *c = &cpu;
    uint32_t entry;
    if (game_boot(c, argv[1], card, NULL, &entry)) { fprintf(stderr, "cannot open disc image\n"); fail_box("cannot open the disc", argv[1]); return 2; }
    rt_set_tick_hook(tick_hook, &h);
    rt_set_fatal_handler(on_fatal);
    if (ticks) rt_set_max_ticks(ticks);
    if (setjmp(g_jb)) { fprintf(stderr, "%s\n", g_msg); rt_hle_card_save(); fail_box("the game stopped", g_msg); SDL_Quit(); return 1; }
    rt_run_entry(c, entry);
    rt_hle_card_save();
    SDL_Quit();
    return 0;
}
