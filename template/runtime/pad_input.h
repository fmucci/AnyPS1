/* Pad input mapping shared by the SDL2 players (keyboard layout file, mouse -> d-pad turning). No SDL here, so it is unit-tested (tests/runtime/test_pad_input.c).
 * Bit map = runtime/hle.c rt_pad_script_add (active low): select 8, L3 9, R3 10, start 11, up 12, right 13, down 14, left 15, L2 0, R2 1, L1 2, R1 3, triangle 4, circle 5, cross 6, square 7.
 * The pad the games read is digital (the BIOS pad word has 16 buttons, no sticks), so a mouse cannot give an analogue camera: horizontal mouse motion is turned into left/right
 * presses whose duty cycle follows the motion (delta-sigma, one decision per virtual frame); the game's own turn speed is the ceiling. */
#ifndef PAD_INPUT_H
#define PAD_INPUT_H
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PADIN_MAXBIND 96
typedef struct { char key[32]; int bit; } PadBind;           /* key: an SDL key name ("W", "Space", "Left Shift"); bit: pad bit */
typedef struct {
    PadBind b[PADIN_MAXBIND]; int n;
    int mouse_on;                                             /* relative mouse -> left/right presses */
    double sens;                                              /* 1.0 = 10 pixels of motion per frame for a full-speed turn */
    int camera;                                               /* 1: mouse sideways turns the player by writing the game's heading (needs a LookProfile); 0: left/right presses */
    int mouse_btn[3];                                         /* pad bit for left / right / middle button, -1 = none */
} PadCfg;

static inline int padin_bit(const char *name) {
    static const struct { const char *n; int bit; } T[] = {{"select", 8}, {"l3", 9}, {"r3", 10}, {"start", 11}, {"up", 12}, {"right", 13}, {"down", 14}, {"left", 15},
        {"l2", 0}, {"r2", 1}, {"l1", 2}, {"r1", 3}, {"triangle", 4}, {"circle", 5}, {"cross", 6}, {"square", 7}, {"none", -1}};
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, name)) return T[i].bit;
    return -2;                                                /* unknown name */
}

static inline void padin_bind(PadCfg *c, const char *key, int bit) {
    for (int i = 0; i < c->n; i++) if (!strcmp(c->b[i].key, key)) { c->b[i].bit = bit; return; }          /* rebinding a key replaces it */
    if (c->n < PADIN_MAXBIND) { snprintf(c->b[c->n].key, sizeof c->b[c->n].key, "%s", key); c->b[c->n].bit = bit; c->n++; }
}

static inline void padin_scheme(PadCfg *c, int wasd) {
    c->n = 0;
    static const char *const common[][2] = {{"Up", "up"}, {"Right", "right"}, {"Down", "down"}, {"Left", "left"}, {"Return", "start"}, {"Right Shift", "select"}, {"Backspace", "select"},
        {"Z", "cross"}, {"X", "circle"}};
    for (unsigned i = 0; i < sizeof common / sizeof common[0]; i++) padin_bind(c, common[i][0], padin_bit(common[i][1]));
    if (wasd) {                                               /* WASD walks; the buttons that were on W/A/S move to the right-hand side of the keyboard */
        static const char *const w[][2] = {{"W", "up"}, {"A", "left"}, {"S", "down"}, {"D", "right"}, {"Q", "square"}, {"E", "circle"}, {"F", "triangle"}, {"Space", "cross"},
            {"1", "r1"}, {"2", "r2"}, {"3", "l1"}, {"4", "l2"}};
        for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) padin_bind(c, w[i][0], padin_bit(w[i][1]));
    } else {                                                  /* the layout of the first player versions */
        static const char *const k[][2] = {{"E", "l2"}, {"R", "r2"}, {"Q", "l1"}, {"W", "r1"}, {"S", "triangle"}, {"A", "square"}};
        for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) padin_bind(c, k[i][0], padin_bit(k[i][1]));
    }
}

static inline void padin_defaults(PadCfg *c) {
    memset(c, 0, sizeof *c);
    padin_scheme(c, 1);
    c->mouse_on = 1; c->sens = 1.0; c->camera = 1;
    c->mouse_btn[0] = padin_bit("square"); c->mouse_btn[1] = padin_bit("triangle"); c->mouse_btn[2] = padin_bit("circle");
}

/* One text line: "scheme wasd|classic", "mouse on|off", "sensitivity 1.5", "key W up" (key names with spaces: "key Left Shift = l1"), "mouse_left square", "unbind W".
 * '#' starts a comment. Returns 0 if the line was understood (or empty), 1 otherwise. */
static inline int padin_line(PadCfg *c, const char *line) {
    char buf[160]; snprintf(buf, sizeof buf, "%s", line);
    char *h = strchr(buf, '#'); if (h) *h = 0;
    for (char *p = buf; *p; p++) if (*p == '=' || *p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
    char *s = buf; while (*s == ' ') s++;
    size_t L = strlen(s); while (L && s[L - 1] == ' ') s[--L] = 0;
    if (!*s) return 0;
    char cmd[32], rest[128] = ""; if (sscanf(s, "%31s %127[^\n]", cmd, rest) < 1) return 1;
    for (char *p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strcmp(cmd, "scheme")) { if (!strcmp(rest, "wasd")) { padin_scheme(c, 1); return 0; } if (!strcmp(rest, "classic")) { padin_scheme(c, 0); return 0; } return 1; }
    if (!strcmp(cmd, "mouse")) { if (!strcmp(rest, "on")) { c->mouse_on = 1; return 0; } if (!strcmp(rest, "off")) { c->mouse_on = 0; return 0; } return 1; }
    if (!strcmp(cmd, "mouse_mode")) { if (!strcmp(rest, "camera")) { c->camera = 1; return 0; } if (!strcmp(rest, "turn")) { c->camera = 0; return 0; } return 1; }
    if (!strcmp(cmd, "sensitivity")) { double v = atof(rest); if (v <= 0.0 || v > 50.0) return 1; c->sens = v; return 0; }
    if (!strncmp(cmd, "mouse_", 6)) {
        int i = !strcmp(cmd + 6, "left") ? 0 : !strcmp(cmd + 6, "right") ? 1 : !strcmp(cmd + 6, "middle") ? 2 : -1;
        int bit = padin_bit(rest); if (i < 0 || bit == -2) return 1; c->mouse_btn[i] = bit; return 0;
    }
    if (!strcmp(cmd, "unbind")) { for (int i = 0; i < c->n; i++) if (!strcmp(c->b[i].key, rest)) { c->b[i] = c->b[--c->n]; return 0; } return 1; }
    if (!strcmp(cmd, "key")) {                                /* the button is the last word, the key name is what is in front of it */
        char *sp = strrchr(rest, ' '); if (!sp) return 1;
        *sp = 0; for (char *e = sp; e > rest && e[-1] == ' '; ) *--e = 0;
        int bit = padin_bit(sp + 1); if (bit < 0 && bit != -1) return 1;
        if (bit == -1) { for (int i = 0; i < c->n; i++) if (!strcmp(c->b[i].key, rest)) { c->b[i] = c->b[--c->n]; return 0; } return 0; }
        padin_bind(c, rest, bit); return 0;
    }
    return 1;
}

/* Parses a whole file's text; returns the number of lines that were not understood (the first is copied to err when given). */
static inline int padin_parse(PadCfg *c, const char *text, char *err, size_t errn) {
    int bad = 0; char line[160]; const char *p = text;
    while (*p) {
        size_t n = strcspn(p, "\n"); if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n); line[n] = 0;
        if (padin_line(c, line)) { if (!bad && err && errn) snprintf(err, errn, "%s", line); bad++; }
        p += strcspn(p, "\n"); if (*p) p++;
    }
    return bad;
}

/* Mouse -> turn. dx is the horizontal motion since the previous frame in pixels (positive = right). Returns an active-low pad word with only the left or right bit cleared. */
typedef struct { double acc; } MouseLook;
#define PADIN_UNIT 10.0                                        /* pixels per frame that keep a direction pressed every frame at sensitivity 1.0 */
static inline uint32_t padin_mouse_turn(MouseLook *m, double dx, double sens) {
    m->acc += dx * sens;
    if (m->acc > 6 * PADIN_UNIT) m->acc = 6 * PADIN_UNIT;       /* a flick does not queue up seconds of turning */
    if (m->acc < -6 * PADIN_UNIT) m->acc = -6 * PADIN_UNIT;
    if (m->acc >= PADIN_UNIT * 0.5) { m->acc -= PADIN_UNIT; return ~(1u << 13); }
    if (m->acc <= -PADIN_UNIT * 0.5) { m->acc += PADIN_UNIT; return ~(1u << 15); }
    return 0xFFFFFFFFu;
}

/* ---- mouse turns the player through the game's memory (per-title profile; the games read a digital pad, so this is a host-side write, findings 0052/0053) ----
 * Observed in the first port (FB2.EXE): the player is an object whose heading is a 10-bit angle (1024 per turn) at +36; the game's camera follows the heading (behind the player, eased), so adding
 * to the heading turns the player on the spot and the view follows. Several globals hold the pointer to the player's object from the moment it exists (zero before); the player is trusted
 * only while three of them agree and point into RAM at a plausible heading, so nothing is written during loading, menus or movies. */
typedef struct { uint32_t ptr_addr[3]; int head_off; int bits; int sign; double scale; int enabled; } LookProfile;   /* scale: angle units per mouse pixel */
typedef struct { double acc; } LookState;
static inline uint32_t padin_rd32(const uint8_t *ram, uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0x1FFFFC), 4); return v; }
static inline void padin_wr32(uint8_t *ram, uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFC), &v, 4); }
/* the player's object address, or 0 */
static inline uint32_t padin_look_player(const LookProfile *p, const uint8_t *ram) {
    uint32_t o = padin_rd32(ram, p->ptr_addr[0]);
    if (o < 0x80100000u || o > 0x801FFF00u || (o & 3)) return 0;
    if (padin_rd32(ram, p->ptr_addr[1]) != o || padin_rd32(ram, p->ptr_addr[2]) != o) return 0;
    if (padin_rd32(ram, o + (uint32_t)p->head_off) >= (1u << p->bits)) return 0;
    return o;
}
/* dx: mouse pixels since the last frame. Returns 1 if memory was written. */
static inline int padin_look_step(const LookProfile *p, LookState *st, uint8_t *ram, double dx, double sens) {
    if (!p->enabled || dx == 0.0) return 0;
    uint32_t o = padin_look_player(p, ram); if (!o) return 0;
    st->acc += dx * sens * p->scale * p->sign; int whole = (int)st->acc; st->acc -= whole;
    if (!whole) return 0;
    uint32_t mask = (1u << p->bits) - 1, h = padin_rd32(ram, o + (uint32_t)p->head_off);
    padin_wr32(ram, o + (uint32_t)p->head_off, (uint32_t)((int)h + whole) & mask);
    return 1;
}

/* Walking relative to the camera without turning the player (finding 0052, addendum 5). The game's d-pad turns the player (tank controls), but it has a side-step: L2 / R2 move him sideways with
 * his heading unchanged, "down" walks backwards facing front, and "up" together with L2/R2 only walks forward (the side-step is ignored). The mouse turns the player (the camera follows, see
 * padin_look_step), so the keys become buttons: W up, S down, A L2, D R2; a diagonal alternates forward and side-step presses (6 ticks forward, 9 ticks side-step in a 15-tick cycle: about 49
 * degrees off forward, measured on the first port). keys: bit0 up, bit1 right, bit2 down, bit3 left. Returns an active-low pad word with only the wanted bits cleared. */
static inline uint32_t padin_walk(int keys, unsigned tick) {
    int fwd = ((keys & 1) ? 1 : 0) - ((keys & 4) ? 1 : 0), side = ((keys & 2) ? 1 : 0) - ((keys & 8) ? 1 : 0);
    int fb = fwd > 0 ? 12 : 14, lat = side < 0 ? 0 : 1;
    if (!fwd && !side) return 0xFFFFFFFFu;
    if (!side) return ~(1u << fb);
    if (!fwd) return ~(1u << lat);
    return (tick % 15) < 6 ? ~(1u << fb) : ~(1u << lat);
}
#endif
