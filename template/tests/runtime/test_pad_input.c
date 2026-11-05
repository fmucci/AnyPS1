/* Unit test of runtime/pad_input.h: key layout file parsing and the mouse -> left/right pulse generator. */
#include <assert.h>
#include <math.h>
#include "../../runtime/pad_input.h"

static int find(const PadCfg *c, const char *key) { for (int i = 0; i < c->n; i++) if (!strcmp(c->b[i].key, key)) return c->b[i].bit; return -99; }

int main(void) {
    PadCfg c; padin_defaults(&c);
    assert(find(&c, "W") == 12 && find(&c, "A") == 15 && find(&c, "S") == 14 && find(&c, "D") == 13);      /* WASD walks */
    assert(find(&c, "Up") == 12 && find(&c, "Z") == 6 && find(&c, "Return") == 11);
    assert(c.mouse_on == 1 && c.mouse_btn[0] == 7 && c.mouse_btn[1] == 5 && c.mouse_btn[2] == 4);
    padin_scheme(&c, 0);                                                                                     /* the first versions' layout */
    assert(find(&c, "W") == 3 && find(&c, "A") == 7 && find(&c, "S") == 4 && find(&c, "D") == -99);

    padin_defaults(&c);
    char err[160] = "";
    const char *txt = "# comment\nscheme wasd\nkey Left Shift = l1\nkey E r2   # trailing comment\nmouse off\nsensitivity 2.5\nmouse_left cross\nunbind Space\n\nbogus line\nkey Q nonsense\n";
    int bad = padin_parse(&c, txt, err, sizeof err);
    assert(bad == 2 && !strcmp(err, "bogus line"));
    assert(find(&c, "Left Shift") == 2 && find(&c, "E") == 1 && find(&c, "Space") == -99 && find(&c, "Q") == 2);
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
    /* camera orbit through the game's memory (fake 2 MiB RAM with the FB2 layout) */
    static uint8_t ram[0x200000];
    LookProfile lp = { 0x800bbe2c, 10, 0x800c7a4c, 0x800bb1a8, 36, -1, 0.35, 0x800e1990, 1 }; LookState ls = {0};
    uint32_t obj = 0x801a6020, decoy = 0x801ac420;
    padin_wr32(ram, 0x800c7a4c, 2); padin_wr32(ram, 0x800bb1a8, 520);
    padin_wr32(ram, obj + 4, 2); padin_wr32(ram, obj + 36, 520);                                                  /* the player */
    padin_wr32(ram, decoy + 4, 258); padin_wr32(ram, decoy + 36, 520);                                            /* same heading, other id */
    assert(!padin_look_step(&lp, &ls, ram, 0.0, 1.0, 0) && padin_rd32(ram, 0x800bbe2c) == 0);                  /* no motion, not walking: nothing written */
    for (int i = 0; i < 100; i++) padin_look_step(&lp, &ls, ram, 10.0, 1.0, 0);                                   /* 1000 px to the right: orbit shrinks by 350 (mod 1024) */
    assert(ls.obj == obj && padin_rd32(ram, 0x800bbe2c) == ((1024 - 350) & 1023) && padin_rd32(ram, obj + 36) == 520);
    uint32_t orbit = padin_rd32(ram, 0x800bbe2c);
    assert(padin_look_step(&lp, &ls, ram, 0.0, 1.0, 1));                                                          /* walking: heading -= orbit, orbit = 0 */
    assert(padin_rd32(ram, 0x800bbe2c) == 0 && padin_rd32(ram, obj + 36) == ((520 - orbit) & 1023) && padin_rd32(ram, decoy + 36) == 520);
    uint32_t yaw_before = ((orbit - 520) & 1023), yaw_after = ((0 - padin_rd32(ram, obj + 36)) & 1023);
    assert(yaw_before == yaw_after);                                                                              /* the view target (orbit - heading) did not change */
    padin_wr32(ram, 0x800bb1a8, padin_rd32(ram, obj + 36));                                                       /* the game refreshes its copy */
    padin_wr32(ram, 0x800c7a4c, 3); assert(!padin_look_step(&lp, &ls, ram, 5.0, 1.0, 0) && ls.obj == 0);          /* the player id changed: the cached object is dropped, no object matches */
    padin_wr32(ram, 0x800c7a4c, 2); padin_wr32(ram, decoy + 4, 2);                                                /* two objects with the id and the same heading: ambiguous, not trusted */
    padin_wr32(ram, decoy + 36, padin_rd32(ram, obj + 36)); ls.obj = 0; assert(!padin_look_step(&lp, &ls, ram, 5.0, 1.0, 0));
    padin_wr32(ram, decoy + 4, 258); assert(padin_look_step(&lp, &ls, ram, 20.0, 1.0, 0) && ls.obj == obj);       /* unique again */
    /* aiming: the lock byte is set, the mouse turns the player and leaves the orbit alone; a pending orbit is committed first */
    uint32_t h0 = padin_rd32(ram, obj + 36); padin_wr32(ram, 0x800bbe2c, 0); ram[0x800e1990 & 0x1FFFFF] = 255;
    for (int i = 0; i < 100; i++) padin_look_step(&lp, &ls, ram, 10.0, 1.0, 0);                                   /* 1000 px right while aiming: heading grows by 350 */
    assert(padin_rd32(ram, 0x800bbe2c) == 0 && padin_rd32(ram, obj + 36) == ((h0 + 350) & 1023));
    padin_wr32(ram, 0x800bbe2c, 100); uint32_t hh = padin_rd32(ram, obj + 36);
    assert(padin_look_step(&lp, &ls, ram, 0.0, 1.0, 0) && padin_rd32(ram, 0x800bbe2c) == 0 && padin_rd32(ram, obj + 36) == ((hh - 100) & 1023));   /* entering the lock commits the orbit */
    ram[0x800e1990 & 0x1FFFFF] = 0;
    lp.enabled = 0; assert(!padin_look_step(&lp, &ls, ram, 50.0, 1.0, 1));
    PadCfg c2; padin_defaults(&c2); assert(c2.camera == 1);
    assert(padin_line(&c2, "mouse_mode turn") == 0 && c2.camera == 0 && padin_line(&c2, "mouse_mode camera") == 0 && c2.camera == 1 && padin_line(&c2, "mouse_mode x") == 1);
    printf("pad_input tests passed\n");
    return 0;
}
