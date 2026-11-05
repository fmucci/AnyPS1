/* Unit test of runtime/pad_input.h: key layout file parsing and the mouse -> left/right pulse generator. */
#include <assert.h>
#include <math.h>
#include "../../runtime/pad_input.h"

static int find(const PadCfg *c, const char *key) { for (int i = 0; i < c->n; i++) if (!strcmp(c->b[i].key, key)) return c->b[i].bit; return -99; }

int main(void) {
    PadCfg c; padin_defaults(&c);
    assert(find(&c, "W") == 12 && find(&c, "A") == 15 && find(&c, "S") == 14 && find(&c, "D") == 13);      /* WASD walks */
    assert(find(&c, "Up") == 12 && find(&c, "Z") == 6 && find(&c, "Return") == 11);
    assert(find(&c, "Q") == 7 && find(&c, "E") == 5 && find(&c, "F") == 4 && find(&c, "Space") == 6);               /* square, circle, triangle, cross */
    assert(find(&c, "1") == 3 && find(&c, "2") == 1 && find(&c, "3") == 2 && find(&c, "4") == 0);                  /* R1, R2, L1, L2 */
    assert(find(&c, "J") == -99 && find(&c, "R") == -99);                                                           /* the earlier letters are free */
    assert(c.mouse_on == 1 && c.mouse_btn[0] == 7 && c.mouse_btn[1] == 4 && c.mouse_btn[2] == 5);
    padin_scheme(&c, 0);                                                                                     /* the first versions' layout */
    assert(find(&c, "W") == 3 && find(&c, "A") == 7 && find(&c, "S") == 4 && find(&c, "D") == -99);

    padin_defaults(&c);
    char err[160] = "";
    const char *txt = "# comment\nscheme wasd\nkey Left Shift = l1\nkey E r2   # trailing comment\nmouse off\nsensitivity 2.5\nmouse_left cross\nunbind Space\n\nbogus line\nkey Q nonsense\n";
    int bad = padin_parse(&c, txt, err, sizeof err);
    assert(bad == 2 && !strcmp(err, "bogus line"));
    assert(find(&c, "Left Shift") == 2 && find(&c, "E") == 1 && find(&c, "Space") == -99 && find(&c, "Q") == 7);
    assert(c.mouse_on == 0 && fabs(c.sens - 2.5) < 1e-9 && c.mouse_btn[0] == 6);
    assert(padin_line(&c, "key Left Shift = none") == 0 && find(&c, "Left Shift") == -99);                      /* 'none' unbinds */
    assert(padin_line(&c, "sensitivity -1") == 1 && padin_line(&c, "mouse maybe") == 1);

    /* mouse look: steady motion of PADIN_UNIT pixels per frame presses every frame, half of it every other frame, none presses nothing */
    MouseLook m = {0};
    int right = 0, left = 0;
    for (int i = 0; i < 100; i++) { uint32_t w = padin_mouse_turn(&m, 10.0, 1.0); assert(!((~w & (1u << 13)) && (~w & (1u << 15)))); right += !(w & (1u << 13)); }
    assert(right >= 99);
    m.acc = 0; right = 0;
    for (int i = 0; i < 100; i++) right += !(padin_mouse_turn(&m, 5.0, 1.0) & (1u << 13));
    assert(right >= 49 && right <= 51);
    m.acc = 0; right = 0;
    for (int i = 0; i < 100; i++) { uint32_t w = padin_mouse_turn(&m, -2.0, 1.0); left += !(w & (1u << 15)); right += !(w & (1u << 13)); }
    assert(right == 0 && left >= 19 && left <= 21);                                                          /* 20 % duty for 2 px per frame */
    m.acc = 0;
    for (int i = 0; i < 100; i++) assert(padin_mouse_turn(&m, 0.0, 1.0) == 0xFFFFFFFFu);
    m.acc = 0; padin_mouse_turn(&m, 100000.0, 1.0);                                                          /* a flick is clamped */
    int tail = 0; for (int i = 0; i < 100; i++) tail += !(padin_mouse_turn(&m, 0.0, 1.0) & (1u << 13));
    assert(tail <= 6);
    /* mouse turns the player through the game's memory (fake 2 MiB RAM with the FB2 layout) */
    static uint8_t ram[0x200000];
    LookProfile lp = { { 0x800b9c10, 0x800b9cc0, 0x800b9c30 }, 36, 10, 1, 0.35, 1 }; LookState ls = {0};
    uint32_t obj = 0x801a6020;
    assert(!padin_look_step(&lp, &ls, ram, 50.0, 1.0));                                                           /* before the player exists (pointers 0): nothing is written */
    padin_wr32(ram, 0x800b9c10, obj); padin_wr32(ram, 0x800b9cc0, obj);
    assert(!padin_look_step(&lp, &ls, ram, 50.0, 1.0) && padin_rd32(ram, obj + 36) == 0);                         /* two of three agree: not trusted */
    padin_wr32(ram, 0x800b9c30, obj); padin_wr32(ram, obj + 36, 520);
    assert(padin_look_player(&lp, ram) == obj);
    assert(!padin_look_step(&lp, &ls, ram, 0.0, 1.0) && padin_rd32(ram, obj + 36) == 520);                        /* no motion, no write */
    for (int i = 0; i < 100; i++) padin_look_step(&lp, &ls, ram, 10.0, 1.0);                                      /* 1000 px to the right: heading grows by 350 */
    assert(padin_rd32(ram, obj + 36) == ((520 + 350) & 1023));
    for (int i = 0; i < 100; i++) padin_look_step(&lp, &ls, ram, -10.0, 2.0);                                     /* sensitivity 2.0 to the left: -700, wraps */
    assert(padin_rd32(ram, obj + 36) == (((520 + 350 - 700) % 1024 + 1024) & 1023));
    padin_wr32(ram, obj + 36, 5000); assert(!padin_look_step(&lp, &ls, ram, 50.0, 1.0));                         /* an implausible heading: not a player object */
    padin_wr32(ram, obj + 36, 100); padin_wr32(ram, 0x800b9c30, obj + 8); assert(!padin_look_step(&lp, &ls, ram, 50.0, 1.0));   /* the pointers disagree */
    padin_wr32(ram, 0x800b9c30, obj); padin_wr32(ram, 0x800b9c10, 0x1f800000); padin_wr32(ram, 0x800b9cc0, 0x1f800000); padin_wr32(ram, 0x800b9c30, 0x1f800000);
    assert(!padin_look_step(&lp, &ls, ram, 50.0, 1.0));                                                           /* all equal but outside the object heap */
    lp.enabled = 0; padin_wr32(ram, 0x800b9c10, obj); padin_wr32(ram, 0x800b9cc0, obj); padin_wr32(ram, 0x800b9c30, obj); assert(!padin_look_step(&lp, &ls, ram, 50.0, 1.0));
    /* walking relative to the camera: keys bit0 up, bit1 right, bit2 down, bit3 left */
    assert(padin_walk(0, 0) == 0xFFFFFFFFu && padin_walk(1 | 4, 7) == 0xFFFFFFFFu && padin_walk(2 | 8, 7) == 0xFFFFFFFFu);   /* nothing, or opposite keys cancelling */
    assert(padin_walk(1, 3) == ~(1u << 12) && padin_walk(4, 3) == ~(1u << 14));                                   /* W up, S down */
    assert(padin_walk(8, 3) == ~(1u << 0) && padin_walk(2, 3) == ~(1u << 1));                                     /* A = L2 (side-step left), D = R2 */
    int fw = 0, sd = 0;                                                                                           /* W+A: forward 6 of every 15 ticks, side-step L2 the other 9 */
    for (unsigned t = 0; t < 150; t++) { uint32_t w = padin_walk(1 | 8, t); assert(w == ~(1u << 12) || w == ~(1u << 0)); fw += w == ~(1u << 12); sd += w == ~(1u << 0); }
    assert(fw == 60 && sd == 90);
    for (unsigned t = 0; t < 15; t++) { uint32_t w = padin_walk(1 | 2, t); assert(w == ~(1u << 12) || w == ~(1u << 1)); }          /* W+D: forward / R2 */
    for (unsigned t = 0; t < 15; t++) { uint32_t w = padin_walk(4 | 8, t); assert(w == ~(1u << 14) || w == ~(1u << 0)); }          /* S+A: back / L2 */
    PadCfg c2; padin_defaults(&c2); assert(c2.camera == 1);
    assert(padin_line(&c2, "mouse_mode turn") == 0 && c2.camera == 0 && padin_line(&c2, "mouse_mode camera") == 0 && c2.camera == 1 && padin_line(&c2, "mouse_mode x") == 1);
    printf("pad_input tests passed\n");
    return 0;
}
