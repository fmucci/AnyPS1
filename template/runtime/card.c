#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "card.h"

#define IMG 131072
#define BLK 8192
#define FR 128

typedef struct { int used, first, nblocks, size, pos, writable, async; } Fcb;       /* async = opened with 0x8000: reads then do not advance the position (finding 0015) */
struct Card {
    uint8_t img[IMG];
    int present, dirty;
    unsigned gen;                                                        /* counts every change of the image: the player writes the file once the game has been quiet for a while (not only when the window closes) */
    char path[512];
    Fcb fcb[16];
    int find_idx, find_deleted, last_error, status, auto_format, delay;     /* delay: frames the events of the last call wait (the reference BIOS moves one 128-byte sector per frame) */
};
static void touch(Card *c) { c->dirty = 1; c->gen++; }
unsigned card_gen(Card *c) { return c ? c->gen : 0; }
int card_event_delay(Card *c) { return c ? c->delay : 0; }

static uint8_t xor_frame(const uint8_t *f) { uint8_t x = 0; for (int i = 0; i < 127; i++) x ^= f[i]; return x; }
static void fix_dir(Card *c, int fr) { c->img[fr * FR + 127] = xor_frame(c->img + fr * FR); }

static void format(Card *c) {
    memset(c->img, 0, IMG);
    c->img[0] = 'M'; c->img[1] = 'C'; c->img[127] = 0x0E;
    for (int f = 1; f <= 15; f++) { c->img[f * FR] = 0xA0; c->img[f * FR + 8] = 0xFF; c->img[f * FR + 9] = 0xFF; fix_dir(c, f); }
    for (int f = 16; f <= 35; f++) { memset(c->img + f * FR, 0, FR); memset(c->img + f * FR, 0xFF, 4); c->img[f * FR + 8] = 0xFF; c->img[f * FR + 9] = 0xFF; fix_dir(c, f); }
    for (int f = 36; f <= 62; f++) memset(c->img + f * FR, 0xFF, FR);
    memcpy(c->img + 63 * FR, c->img, FR);
    touch(c);
}
static int formatted(Card *c) { return c->img[0] == 'M' && c->img[1] == 'C'; }

Card *card_new(const char *path) {
    Card *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->status = 0x11;
    if (!path) return c;
    c->present = 1; c->status = 1;
    snprintf(c->path, sizeof c->path, "%s", path);
    FILE *f = fopen(path, "rb");
    if (f) { size_t n = fread(c->img, 1, IMG, f); fclose(f); if (n != IMG) memset(c->img, 0, IMG); c->dirty = 0; }
    else format(c);                                                       /* a new card stays dirty: the player writes the (empty, formatted) file a moment after the start, so it exists even if the process is killed before the first save */
    return c;
}
void card_set_path(Card *c, const char *path) { if (c && path) snprintf(c->path, sizeof c->path, "%s", path); }
void card_free(Card *c) { if (c) { card_save(c); free(c); } }
int card_save(Card *c) {                                                  /* atomic: a temporary file next to the card, then renamed over it, so a kill or power loss never leaves a half-written card */
    if (!c || !c->present || !c->dirty) return 1;
    char tmp[sizeof c->path + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", c->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(c->img, 1, IMG, f) == IMG;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
#ifdef _WIN32
        remove(c->path);                                                  /* rename does not replace an existing file on Windows */
#endif
        ok = rename(tmp, c->path) == 0;
    }
    if (ok) c->dirty = 0; else remove(tmp);
    return ok;
}
const uint8_t *card_image(Card *c) { return c->img; }

/* ---- guest memory helpers (addresses masked into the 2 MiB RAM) ---- */
static uint8_t *gp(uint8_t *ram, uint32_t a, uint32_t n) { uint32_t o = a & 0x1FFFFF; return (o + n <= 0x200000) ? ram + o : NULL; }
static void put32(uint8_t *ram, uint32_t a, uint32_t v) { uint8_t *p = gp(ram, a, 4); if (p) memcpy(p, &v, 4); }
static int gstr(uint8_t *ram, uint32_t a, char *out, int max) {
    for (int i = 0; i < max - 1; i++) { uint8_t *p = gp(ram, a + i, 1); if (!p) return 0; out[i] = (char)*p; if (!*p) return 1; }
    out[max - 1] = 0;
    return 1;
}

/* ---- directory ---- */
static int dstate(Card *c, int b) { return c->img[b * FR]; }
static int match(const char *pat, const char *name) {          /* '*' ignores the rest, '?' one character (psx-spx firstfile) */
    for (int i = 0; i < 20; i++) {
        if (pat[i] == '*') return 1;
        if (pat[i] == '?') { if (name[i] == 0) return 1; continue; }   /* documented BUG: '?' matching the end ignores the rest */
        if (pat[i] != name[i]) return 0;
        if (!pat[i]) return 1;
    }
    return 1;
}
static void fname(Card *c, int b, char out[21]) { memcpy(out, c->img + b * FR + 10, 20); out[20] = 0; }
static int find_file(Card *c, const char *pat, int deleted) {
    for (int b = 1; b <= 15; b++) {
        int s = dstate(c, b);
        if (s != (deleted ? 0xA1 : 0x51)) continue;
        char nm[21]; fname(c, b, nm);
        if (match(pat, nm)) return b;
    }
    return 0;
}
static int next_block(Card *c, int b) { int n = c->img[b * FR + 8] | (c->img[b * FR + 9] << 8); return n == 0xFFFF ? 0 : n + 1; }

/* "bu00:NAME" -> NAME; returns -1 unless the device is the first card slot */
static int split_device(const char *full, const char **rest) {
    if (full[0] == 'b' && full[1] == 'u' && full[4] == ':') {
        int port = ((full[2] >= '0' && full[2] <= '9') ? full[2] - '0' : 0) * 16 + ((full[3] >= '0' && full[3] <= '9') ? full[3] - '0' : 0);
        *rest = full + 5;
        return port;
    }
    return -1;
}

static void evt(uint32_t ev[8][2], int *n, uint32_t cls, uint32_t spec) { if (*n < 8) { ev[*n][0] = cls; ev[*n][1] = spec; (*n)++; } }
static void done_ok(Card *c, uint32_t ev[8][2], int *n, uint32_t cls) { c->status = 1; evt(ev, n, cls, 0x0004); }
static void fail_nocard(Card *c, uint32_t ev[8][2], int *n, uint32_t cls) { c->status = 0x11; evt(ev, n, cls, 0x0100); }

#define HL 0xF4000001u      /* higher level file/device events */
#define LL 0xF0000011u      /* lower level card events */

int card_call(Card *c, uint8_t *ram, char t, unsigned n, const uint32_t a[4], uint32_t *ret, uint32_t ev[8][2], int *nev) {
    *nev = 0; *ret = 0; c->delay = 0;
    if (t == 'A' && (n == 0x55 || n == 0x70)) { return 1; }                    /* _bu_init */
    if (t == 'B' && (n == 0x4A || n == 0x4B || n == 0x4C)) return 1;           /* InitCard / StartCard / StopCard */
    if (t == 'B' && n == 0x50) return 1;                                       /* allow_new_card (the card-changed flag is not modelled) */
    if (t == 'A' && n == 0xAD) { c->auto_format = a[0] & 1; return 1; }
    if (t == 'B' && n == 0x5C) { *ret = (uint32_t)c->status; return 1; }       /* get_card_status */
    if (t == 'B' && n == 0x5D) { *ret = (uint32_t)c->status; return 1; }       /* wait_card_status: operations are instantaneous */
    if (t == 'A' && n == 0xAB) {                                               /* _card_info(port): the program tests the high level events (SwCARD F4000001); slot 2 (port bit 4) has no card */
        *ret = 1;
        if (c->present && !(a[0] & 0x10)) done_ok(c, ev, nev, HL); else fail_nocard(c, ev, nev, HL);
        return 1;
    }
    if (t == 'B' && n == 0x4D) {                                               /* low level card info: completion via a low level event */
        *ret = 1;
        if (c->present) done_ok(c, ev, nev, LL); else fail_nocard(c, ev, nev, LL);
        return 1;
    }
    if (t == 'A' && n == 0xAC) {                                               /* _card_async_load_directory */
        *ret = 1;
        if (!c->present) fail_nocard(c, ev, nev, HL);
        else if (!formatted(c)) { c->status = 0x21; evt(ev, nev, HL, 0x2000); }
        else { done_ok(c, ev, nev, HL); evt(ev, nev, LL, 0x0004); c->delay = 2; }                 /* the reference BIOS ends a load with the SwCARD event and then the HwCARD one */
        return 1;
    }
    if (t == 'B' && (n == 0x4E || n == 0x4F)) {                                 /* write/read_card_sector(port, sector, buf) */
        uint32_t port = a[0], sec = a[1];
        if (sec > 0x400) { *ret = 0; return 1; }
        *ret = 1;
        if (!c->present || (port & 0xF0)) { fail_nocard(c, ev, nev, LL); return 1; }
        uint8_t *buf = gp(ram, a[2], FR);
        if (!buf || sec * FR + FR > IMG) { c->status = 0x21; evt(ev, nev, LL, 0x8000); return 1; }
        if (n == 0x4F) memcpy(buf, c->img + sec * FR, FR);
        else { memcpy(c->img + sec * FR, buf, FR); touch(c); }
        done_ok(c, ev, nev, LL);
        return 1;
    }
    if (t == 'B' && n == 0x32) {                                               /* FileOpen(name, mode) */
        char nm[64]; const char *rest;
        if (!gstr(ram, a[0], nm, sizeof nm)) { *ret = (uint32_t)-1; return 1; }
        int port = split_device(nm, &rest);
        if (port != 0 || !c->present || !formatted(c)) { c->last_error = 5; *ret = (uint32_t)-1; fail_nocard(c, ev, nev, HL); return 1; }
        int fd = -1;
        for (int i = 2; i < 16; i++) if (!c->fcb[i].used) { fd = i; break; }
        if (fd < 0) { c->last_error = 0x18; *ret = (uint32_t)-1; return 1; }
        int b = find_file(c, rest, 0);
        if (a[1] & 0x200) {                                                    /* create */
            unsigned nb = a[1] >> 16;
            if (b || nb == 0 || nb > 15) { c->last_error = b ? 0x11 : 0x16; *ret = (uint32_t)-1; evt(ev, nev, HL, 0x8000); return 1; }
            int blocks[15], got = 0;
            for (int k = 1; k <= 15 && got < (int)nb; k++) if ((dstate(c, k) & 0xF0) == 0xA0) blocks[got++] = k;
            if (got < (int)nb) { c->last_error = 0x1C; *ret = (uint32_t)-1; evt(ev, nev, HL, 0x8000); return 1; }
            for (int k = 0; k < got; k++) {
                uint8_t *f = c->img + blocks[k] * FR;
                memset(f, 0, FR);
                f[0] = got == 1 || k == 0 ? 0x51 : (k == got - 1 ? 0x53 : 0x52);
                if (k == 0) { uint32_t sz = nb * BLK; memcpy(f + 4, &sz, 4); strncpy((char *)f + 10, rest, 20); }
                int nx = k + 1 < got ? blocks[k + 1] - 1 : 0xFFFF;
                f[8] = (uint8_t)nx; f[9] = (uint8_t)(nx >> 8);
                fix_dir(c, blocks[k]);
                memset(c->img + blocks[k] * BLK, 0, BLK);
            }
            touch(c);
            b = blocks[0];
            c->fcb[fd] = (Fcb){1, b, (int)nb, (int)(nb * BLK), 0, 1, (a[1] & 0x8000) != 0};
        } else {
            if (!b) { c->last_error = 2; *ret = (uint32_t)-1; evt(ev, nev, HL, 0x8000); return 1; }
            uint32_t sz; memcpy(&sz, c->img + b * FR + 4, 4);
            c->fcb[fd] = (Fcb){1, b, (int)(sz / BLK), (int)sz, 0, 1, (a[1] & 0x8000) != 0};
        }
        *ret = (uint32_t)fd;
        if (a[1] & 0x8000) done_ok(c, ev, nev, HL);
        return 1;
    }
    if (t == 'B' && (n == 0x33 || n == 0x34 || n == 0x35 || n == 0x36)) {
        uint32_t fd = a[0];
        if (n == 0x35 && fd == 1) return 0;                                    /* stdout: handled by the host */
        if (fd < 2 || fd > 15 || !c->fcb[fd].used) { c->last_error = 9; *ret = (uint32_t)-1; return 1; }
        Fcb *f = &c->fcb[fd];
        if (n == 0x36) { f->used = 0; *ret = fd; done_ok(c, ev, nev, HL); return 1; }
        if (n == 0x33) {
            int32_t off = (int32_t)a[1];
            int np = a[2] == 0 ? off : f->pos + off;
            if (a[2] > 1 || np < 0) { c->last_error = 0x16; *ret = (uint32_t)-1; return 1; }
            f->pos = np; *ret = (uint32_t)np; done_ok(c, ev, nev, HL); return 1;
        }
        uint32_t len = a[2];
        uint8_t *buf = gp(ram, a[1], len);
        if (!buf) { c->last_error = 0x16; *ret = (uint32_t)-1; return 1; }
        int remain = f->size - f->pos;
        if (remain < 0) remain = 0;
        uint32_t cnt = len > (uint32_t)remain ? (uint32_t)remain : len;
        for (uint32_t k = 0; k < cnt; k++) {
            int p = f->pos + (int)k, blk = f->first;
            for (int s = p / BLK; s > 0 && blk; s--) blk = next_block(c, blk);
            if (!blk) { cnt = k; break; }
            uint8_t *cell = c->img + blk * BLK + (p % BLK);
            if (n == 0x34) buf[k] = *cell; else { *cell = buf[k]; touch(c); }
        }
        if (!(f->async && n == 0x34)) f->pos += (int)cnt;                         /* the reference BIOS leaves the position of an async read alone ("?? ff9" in its source: written for a title that needed it): the program seeks itself */
        *ret = cnt;
        if (f->async) c->delay = (int)((cnt + 127) >> 7) + 1;
        done_ok(c, ev, nev, HL);
        return 1;
    }
    if (t == 'B' && n == 0x41) {                                               /* FormatDevice */
        char nm[32]; const char *rest;
        if (gstr(ram, a[0], nm, sizeof nm) && split_device(nm, &rest) == 0 && c->present) { format(c); *ret = 1; done_ok(c, ev, nev, HL); }
        else { *ret = 0; fail_nocard(c, ev, nev, HL); }
        return 1;
    }
    if (t == 'B' && (n == 0x42 || n == 0x43)) {                                /* firstfile(pattern, direntry) / nextfile(direntry) */
        char nm[64]; const char *rest = "";
        uint32_t de = a[n == 0x42 ? 1 : 0];
        static char pat[24];
        if (n == 0x42) {
            if (!gstr(ram, a[0], nm, sizeof nm) || split_device(nm, &rest) != 0 || !c->present || !formatted(c)) { *ret = 0; return 1; }
            memset(pat, 0, sizeof pat); strncpy(pat, rest, 20);
            c->find_idx = 1;
        }
        for (; c->find_idx <= 15; c->find_idx++) {
            int b = c->find_idx;
            if (dstate(c, b) != (c->find_deleted ? 0xA1 : 0x51)) continue;
            char fn[21]; fname(c, b, fn);
            if (!match(pat, fn)) continue;
            uint8_t ent[0x28] = {0};
            memcpy(ent, fn, 20);
            uint32_t attr = 0x50, sz; memcpy(&sz, c->img + b * FR + 4, 4);
            memcpy(ent + 0x14, &attr, 4); memcpy(ent + 0x18, &sz, 4);
            uint32_t fs = (uint32_t)b; memcpy(ent + 0x20, &fs, 4);
            uint8_t *d = gp(ram, de, sizeof ent);
            if (d) memcpy(d, ent, sizeof ent);
            c->find_idx++;
            *ret = de;
            return 1;
        }
        *ret = 0;
        return 1;
    }
    if (t == 'B' && n == 0x45) {                                               /* FileDelete */
        char nm[64]; const char *rest;
        if (!gstr(ram, a[0], nm, sizeof nm) || split_device(nm, &rest) != 0 || !c->present || !formatted(c)) { *ret = 0; return 1; }
        int b = find_file(c, rest, 0);
        if (!b) { c->last_error = 2; *ret = 0; evt(ev, nev, HL, 0x8000); return 1; }
        while (b) {
            int nx = next_block(c, b);
            c->img[b * FR] = (uint8_t)(c->img[b * FR] + 0x50);               /* 51h->A1h, 52h->A2h, 53h->A3h */
            fix_dir(c, b);
            b = nx;
        }
        touch(c);
        *ret = 1;
        done_ok(c, ev, nev, HL);
        return 1;
    }
    if (t == 'B' && (n == 0x54 || n == 0x55)) { *ret = (uint32_t)c->last_error; return 1; }
    (void)put32;
    return 0;
}
