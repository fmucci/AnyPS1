/* Tests of the virtual memory card and its BIOS calls (queue P3-3): format, create/write/read/seek/close, firstfile/nextfile with wildcards,
 * delete, sector access, persistence in the backing file, no-card behaviour. Guest RAM is a plain 2 MiB buffer. */
#include <stdio.h>
#include <stdlib.h>
#include "tmp_path.h"
#include <string.h>
#include "card.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static uint8_t *ram;
static void gstr(uint32_t a, const char *s) { strcpy((char *)ram + (a & 0x1FFFFF), s); }
static uint32_t call(Card *c, char t, unsigned n, uint32_t a0, uint32_t a1, uint32_t a2, int *nev, uint32_t ev[8][2]) {
    uint32_t a[4] = {a0, a1, a2, 0}, ret = 0xDEAD;
    int h = card_call(c, ram, t, n, a, &ret, ev, nev);
    CHECK(h == 1);
    return ret;
}

int main(void) {
    ram = calloc(1, 0x200000);
    char path[512]; tmp_path(path, sizeof path, "card");
    uint32_t ev[8][2]; int nev;
    Card *c = card_new(path);
    const uint8_t *img = card_image(c);
    CHECK(img[0] == 'M' && img[1] == 'C' && img[127] == 0x0E && img[128] == 0xA0);        /* freshly formatted */
    /* create a 2-block file */
    gstr(0x80100000, "bu00:BASLUS-00236TEST");
    uint32_t fd = call(c, 'B', 0x32, 0x80100000, 0x200 | (2u << 16) | 0x8000, 0, &nev, ev);
    CHECK(fd == 2 && nev == 1 && ev[0][0] == 0xF4000001u && ev[0][1] == 4);
    CHECK(img[128] == 0x51 && img[256] == 0x53 && img[128 + 8] == 1 && img[128 + 9] == 0 && img[256 + 8] == 0xFF);   /* chain 1 -> 2 */
    CHECK(strcmp((const char *)img + 128 + 10, "BASLUS-00236TEST") == 0);
    CHECK(img[128 + 127] == (uint8_t)({ uint8_t x = 0; for (int i = 0; i < 127; i++) x ^= img[128 + i]; x; }));
    uint32_t sz; memcpy(&sz, img + 128 + 4, 4); CHECK(sz == 0x4000);
    for (int i = 0; i < 0x2100; i++) ram[0x300000 / 2 + i] = (uint8_t)(i * 7);
    CHECK(call(c, 'B', 0x35, fd, 0x80000000u + 0x300000 / 2, 0x2100, &nev, ev) == 0x2100);   /* crosses a block boundary */
    CHECK(call(c, 'B', 0x33, fd, 0x1FF0, 0, &nev, ev) == 0x1FF0);
    CHECK(call(c, 'B', 0x34, fd, 0x80180000, 0x30, &nev, ev) == 0x30);
    CHECK(memcmp(ram + 0x180000, ram + 0x300000 / 2 + 0x1FF0, 0x30) == 0);
    CHECK(call(c, 'B', 0x33, fd, 0x3FF0, 0, &nev, ev) == 0x3FF0);
    CHECK(call(c, 'B', 0x34, fd, 0x80180000, 0x100, &nev, ev) == 0x10);                       /* clamped at the end of the file */
    CHECK(call(c, 'B', 0x36, fd, 0, 0, &nev, ev) == fd);
    CHECK(call(c, 'B', 0x34, fd, 0x80180000, 4, &nev, ev) == (uint32_t)-1);                   /* closed handle */
    /* second file, then directory scan with wildcards */
    gstr(0x80100000, "bu00:BASLUS-00236OTHER");
    uint32_t f2 = call(c, 'B', 0x32, 0x80100000, 0x200 | (1u << 16), 0, &nev, ev);
    CHECK(f2 == 2); call(c, 'B', 0x36, f2, 0, 0, &nev, ev);
    gstr(0x80100000, "bu00:BASLUS-00236*");
    uint32_t de = call(c, 'B', 0x42, 0x80100000, 0x80110000, 0, &nev, ev);
    CHECK(de == 0x80110000 && strcmp((char *)ram + 0x110000, "BASLUS-00236TEST") == 0);
    uint32_t a1; memcpy(&a1, ram + 0x110018, 4); CHECK(a1 == 0x4000);
    CHECK(call(c, 'B', 0x43, 0x80110000, 0, 0, &nev, ev) == 0x80110000 && strcmp((char *)ram + 0x110000, "BASLUS-00236OTHER") == 0);
    CHECK(call(c, 'B', 0x43, 0x80110000, 0, 0, &nev, ev) == 0);
    gstr(0x80100000, "bu00:BESLUS*");
    CHECK(call(c, 'B', 0x42, 0x80100000, 0x80110000, 0, &nev, ev) == 0);
    /* errors */
    gstr(0x80100000, "bu00:BASLUS-00236TEST");
    CHECK(call(c, 'B', 0x32, 0x80100000, 0x200 | (1u << 16), 0, &nev, ev) == (uint32_t)-1 && nev == 1 && ev[0][1] == 0x8000);   /* exists */
    gstr(0x80100000, "bu10:X");
    CHECK(call(c, 'B', 0x32, 0x80100000, 1, 0, &nev, ev) == (uint32_t)-1);                    /* slot 2 has no card */
    /* delete, reopen fails, space reusable */
    gstr(0x80100000, "bu00:BASLUS-00236TEST");
    CHECK(call(c, 'B', 0x45, 0x80100000, 0, 0, &nev, ev) == 1 && img[128] == 0xA1 && img[256] == 0xA3);
    CHECK(call(c, 'B', 0x32, 0x80100000, 1, 0, &nev, ev) == (uint32_t)-1);
    /* sector API */
    for (int i = 0; i < 128; i++) ram[0x200 + i] = (uint8_t)i;
    CHECK(call(c, 'B', 0x4E, 0, 0x3F, 0x80000200, &nev, ev) == 1 && nev == 1 && ev[0][0] == 0xF0000011u && ev[0][1] == 4);
    CHECK(call(c, 'B', 0x4F, 0, 0x3F, 0x80000400, &nev, ev) == 1 && memcmp(ram + 0x400, ram + 0x200, 128) == 0);
    CHECK(call(c, 'B', 0x4F, 0, 0x401, 0x80000400, &nev, ev) == 0);                           /* out of range */
    /* persistence: the generation counter moves with every change and the save is atomic (no leftover temporary file, the file equals the image) */
    unsigned g0 = card_gen(c);
    CHECK(call(c, 'B', 0x4E, 0, 0x3E, 0x80000200, &nev, ev) == 1 && card_gen(c) != g0);
    CHECK(card_save(c));
    { char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *tf = fopen(tmp, "rb"); CHECK(tf == NULL); if (tf) fclose(tf);
      uint8_t disk[131072]; FILE *cf = fopen(path, "rb"); CHECK(cf && fread(disk, 1, sizeof disk, cf) == sizeof disk && memcmp(disk, card_image(c), sizeof disk) == 0); if (cf) fclose(cf); }
    card_free(c);
    c = card_new(path);
    img = card_image(c);
    CHECK(img[0] == 'M' && img[256] == 0xA3 && memcmp(img + 63 * 128, ram + 0x200, 128) == 0);
    /* format */
    gstr(0x80100000, "bu00:");
    CHECK(call(c, 'B', 0x41, 0x80100000, 0, 0, &nev, ev) == 1 && img[128] == 0xA0);
    card_free(c);
    remove(path);
    /* no card */
    c = card_new(NULL);
    gstr(0x80100000, "bu00:X");
    CHECK(call(c, 'B', 0x32, 0x80100000, 1, 0, &nev, ev) == (uint32_t)-1 && nev == 1 && ev[0][1] == 0x100);
    CHECK(call(c, 'A', 0xAB, 0, 0, 0, &nev, ev) == 1 && nev == 1 && ev[0][0] == 0xF4000001u && ev[0][1] == 0x100);
    card_free(c);
    c = card_new(path);                                                         /* _card_info: slot 1 with a card -> SwCARD "ok", slot 2 (port 0x10) -> SwCARD "no card" (the program tests the F4000001 events, as in the reference BIOS) */
    CHECK(call(c, 'A', 0xAB, 0, 0, 0, &nev, ev) == 1 && nev == 1 && ev[0][0] == 0xF4000001u && ev[0][1] == 0x4);
    CHECK(call(c, 'A', 0xAB, 0x10, 0, 0, &nev, ev) == 1 && nev == 1 && ev[0][0] == 0xF4000001u && ev[0][1] == 0x100);
    card_free(c); remove(path);
    c = card_new(NULL);
    CHECK(call(c, 'B', 0x5C, 0, 0, 0, &nev, ev) == 0x11);
    { uint32_t a[4] = {0}, r, e[8][2]; int n; CHECK(card_call(c, ram, 'B', 0x56, a, &r, e, &n) == 0); }   /* not a card call */
    card_free(c);
    /* async file reads (open flag 0x8000) leave the position alone, the program seeks itself; events wait one frame per 128-byte sector (reference BIOS, finding 0015) */
    c = card_new(path);
    gstr(0x80100000, "bu00:ASYNC");
    fd = call(c, 'B', 0x32, 0x80100000, 0x200 | (1u << 16) | 0x8000, 0, &nev, ev);
    for (int i = 0; i < 0x300; i++) ram[0x300000 / 2 + i] = (uint8_t)(i * 3 + 1);
    CHECK(call(c, 'B', 0x35, fd, 0x80000000u + 0x300000 / 2, 0x300, &nev, ev) == 0x300 && card_event_delay(c) == 7);     /* 6 sectors + 1 */
    call(c, 'B', 0x33, fd, 0, 0, &nev, ev);
    CHECK(call(c, 'B', 0x34, fd, 0x80180000, 0x80, &nev, ev) == 0x80 && card_event_delay(c) == 2 && memcmp(ram + 0x180000, ram + 0x300000 / 2, 0x80) == 0);
    CHECK(call(c, 'B', 0x34, fd, 0x80180100, 0x80, &nev, ev) == 0x80 && memcmp(ram + 0x180100, ram + 0x300000 / 2, 0x80) == 0);   /* position did not advance */
    call(c, 'B', 0x33, fd, 0x80, 1, &nev, ev);
    CHECK(call(c, 'B', 0x34, fd, 0x80180200, 0x80, &nev, ev) == 0x80 && memcmp(ram + 0x180200, ram + 0x300000 / 2 + 0x80, 0x80) == 0);
    CHECK(call(c, 'A', 0xAC, 0, 0, 0, &nev, ev) == 1 && nev == 2 && ev[0][0] == 0xF4000001u && ev[1][0] == 0xF0000011u && card_event_delay(c) == 2);   /* _card_load: SwCARD then HwCARD, 3 frames */
    card_set_load_frames(c, 72);                                                  /* a game whose menu needs the long load (docs/11): SwCARD after 72 frames */
    CHECK(card_load_frames(c) == 72 && call(c, 'A', 0xAC, 0, 0, 0, &nev, ev) == 1 && nev == 2 && card_event_delay(c) == 72);
    card_set_load_frames(c, 0);
    CHECK(card_load_frames(c) == 0 && call(c, 'A', 0xAC, 0, 0, 0, &nev, ev) == 1 && card_event_delay(c) == 2);
    card_free(c); remove(path);
    printf("card tests: %d checks, %d failures\n", checks, fails);
    free(ram);
    return fails != 0;
}
