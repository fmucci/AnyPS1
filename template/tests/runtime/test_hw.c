/* Unit tests of the hardware models in runtime/io.c that the recompiled game depends on (queue P4-2..P4-5): SPU (ADPCM, ADSR, noise, XA),
 * CD-ROM controller (Setloc/ReadN/data FIFO), GPU rasteriser (flat triangle, rectangle, VRAM fill). Expected values are derived by hand from the
 * psx-spx formulas quoted at each check, not by running the code under test. The file includes io.c so the static helpers are reachable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tmp_path.h"
#include "../../runtime/io.c"

#include "../../runtime/rt.h"
/* the interpreter tier (runtime/interp.c) counts virtual time through rt_yield, which lives in hle.c; these unit tests do not link it */
void rt_yield(Cpu *c) { c->yield = RT_YIELD_PERIOD; }
void rt_dma_kick(Cpu *c) { (void)c; }

static int g_fail, g_n;
#define CHECK(cond, ...) do { g_n++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t g_ram[0x200000];
static IoCtx *mk(void) { memset(g_ram, 0, sizeof g_ram); return io_new(g_ram); }
static void w16(IoCtx *x, uint32_t p, uint32_t v) { int ok = 1; io_write(x, p, 2, v, &ok); CHECK(ok, "write %08x: %s", p, io_last_error(x)); }
static void w32(IoCtx *x, uint32_t p, uint32_t v) { int ok = 1; io_write(x, p, 4, v, &ok); CHECK(ok, "write %08x: %s", p, io_last_error(x)); }
static void w8(IoCtx *x, uint32_t p, uint32_t v) { int ok = 1; io_write(x, p, 1, v, &ok); CHECK(ok, "write %08x: %s", p, io_last_error(x)); }
static uint32_t r8(IoCtx *x, uint32_t p) { int ok = 1; uint32_t v = io_read(x, p, 1, &ok); CHECK(ok, "read %08x", p); return v; }

static void test_adpcm_and_adsr(void) {
    IoCtx *x = mk();
    /* one SPU-ADPCM block at byte address 0x1000: header shift=12 filter=0, flags=loop start+end+repeat (7), nibbles 1,2,3,... */
    uint8_t *b = x->spuram + 0x1000;
    b[0] = 12; b[1] = 7;
    for (int j = 0; j < 14; j++) b[2 + j] = (uint8_t)((((2 * j + 2) & 15) << 4) | ((2 * j + 1) & 15));
    memset(x->spuram + 0x1010, 0, 16); x->spuram[0x1011] = 7;     /* a silent looping block after it */
    /* decoded samples: psx-spx: (nibble << 12) >> shift with shift 12 = the nibble value itself for filter 0 */
    x->spu_r[0x16 >> 1] = 0x1000 / 8;                               /* voice 1 start address register (1C16h), in 8-byte units */
    spu_keyon(x, 1);
    Voice *v = &x->spu.v[1];
    int exp_s[8] = {1, 2, 3, 4, 5, 6, 7, -8};                       /* nibbles 1..7 then 8 (signed -8) */
    for (int k = 0; k < 8; k++) CHECK(v->s[3 + k] == exp_s[k], "sample %d = %d, want %d", k, v->s[3 + k], exp_s[k]);
    /* filter 1: s = t + (old*60 + 32) / 64 (arithmetic shift of the sum) */
    b[0] = 12 | (1 << 4);
    memset(b + 2, 0, 14); b[2] = 0x04; b[3] = 0x00;                   /* sample0 = 4, then zeros: each next = old*60/64 rounded as in the formula */
    spu_keyon(x, 1);
    int old = 4, e1 = (old * 60 + 32) >> 6;
    CHECK(v->s[3] == 4 && v->s[4] == e1, "filter1 sample1 %d want %d", v->s[4], e1);
    /* ADSR: attack linear shift 0 step 7 (A1 = 0x0000 -> mode 0, shift 0, step "+7"): level += 7 << 11 per 44.1 kHz cycle, saturating at 7FFF */
    x->spu_r[(0x1C08 - 0x1C00) >> 1] = 0x0000; x->spu_r[(0x1C0A - 0x1C00) >> 1] = 0x0000;
    Voice *a = &x->spu.v[2];
    a->on = 1; a->phase = 0; a->level = 0; a->adsr_cnt = 0;
    int lv1 = spu_adsr_step(x, 2, a), lv2 = spu_adsr_step(x, 2, a), lv3 = spu_adsr_step(x, 2, a);
    CHECK(lv1 == 14336 && lv2 == 28672 && lv3 == 32767, "attack levels %d %d %d", lv1, lv2, lv3);
    CHECK(a->phase == 1, "attack ends in decay (phase %d)", a->phase);
    io_free(x);
}

static void test_adsr_release(void) {
    /* finding 0045: the game treats a voice as free only when its current ADSR volume reads 0, so a release has to reach exactly 0 and has to start at once */
    IoCtx *x = mk();
    Voice *v = &x->spu.v[3];
    /* A2 = 0xCFA5: sustain exponential decrease, shift 15 (slow); release exponential (bit 5), shift 5 */
    x->spu_r[(0x1C38 - 0x1C00) >> 1] = 0x00FF; x->spu_r[(0x1C3A - 0x1C00) >> 1] = 0xCFA5;
    v->on = 1; v->phase = 2; v->level = 0x7FFF; v->adsr_cnt = 1 << 20;      /* mid-way through a ~1M-sample wait of a very slow sustain */
    w16(x, 0x1F801D8C, 1 << 3);                                              /* key off voice 3 */
    CHECK(v->phase == 3 && v->adsr_cnt == 0, "key off starts the release at once (phase %d, counter %d)", v->phase, v->adsr_cnt);
    int n = 0; while (v->on && n < 2000000) { spu_adsr_step(x, 3, v); n++; }
    CHECK(!v->on && v->level == 0, "an exponential release ends at level 0 (on %d, level %d after %d cycles)", v->on, v->level, n);
    /* a small level still moves down under the exponential rule */
    Voice *u = &x->spu.v[4];
    x->spu_r[(0x1C48 - 0x1C00) >> 1] = 0x00FF; x->spu_r[(0x1C4A - 0x1C00) >> 1] = 0x0025;       /* release: exponential, shift 5 */
    u->on = 1; u->phase = 3; u->level = 20; u->adsr_cnt = 0;
    int l0 = u->level; spu_adsr_step(x, 4, u);
    CHECK(u->level < l0, "level 20 decreases in an exponential release (%d -> %d)", l0, u->level);
    io_free(x);
}

static void test_spu_output(void) {
    IoCtx *x = mk();
    /* voice 0 plays a constant block (shift 0 so nibble 7 -> 7<<12 = 28672) at pitch 0x1000 (44100 Hz), ADSR already at max, full volume */
    uint8_t *b = x->spuram + 0x2000;
    b[0] = 0; b[1] = 3; memset(b + 2, 0x77, 14);
    memcpy(x->spuram + 0x2010, b, 16);
    w16(x, 0x1F801DAA, 0xC000);                                      /* SPU enabled, not muted */
    w16(x, 0x1F801D80, 0x3FFF); w16(x, 0x1F801D82, 0x3FFF);          /* main volume ~ 0.5 */
    w16(x, 0x1F801C00, 0x3FFF); w16(x, 0x1F801C02, 0x3FFF);          /* voice volume ~ 0.5 */
    w16(x, 0x1F801C04, 0x1000); w16(x, 0x1F801C06, 0x2000 / 8);
    w16(x, 0x1F801C08, 0x00FF); w16(x, 0x1F801C0A, 0x0000);         /* attack: exponential no, shift 0, step 4 -> fast; sustain mode irrelevant */
    w16(x, 0x1F801D88, 1);                                           /* key on voice 0 */
    uint64_t before = io_spu_total(x);
    for (int i = 0; i < 4; i++) io_vblank_tick(x);
    CHECK(io_spu_total(x) - before == 4 * 735, "735 frames per virtual frame, got %llu", (unsigned long long)(io_spu_total(x) - before));
    int16_t buf[2 * 4096]; int n = io_spu_pull(x, buf, 4096);
    CHECK(n == 4 * 735, "pulled %d", n);
    int peak = 0; for (int i = 0; i < 2 * n; i++) { int a = abs(buf[i]); if (a > peak) peak = a; }
    /* volume registers hold volume/2 (psx-spx): 0x3FFF = +7FFE = full scale, so the peak is 28672 * env * vol * main, about 28672 less the Gaussian sum loss (0x7F7F/0x8000) */
    CHECK(peak > 28000 && peak < 28700, "steady-state peak %d, want about 28500", peak);
    CHECK(buf[2 * 100] == buf[2 * 100 + 1], "left and right equal for equal volumes");
    /* the mute bit (SPUCNT bit 14 = 0) silences the mix */
    w16(x, 0x1F801DAA, 0x8000);
    for (int i = 0; i < 2; i++) io_vblank_tick(x);
    n = io_spu_pull(x, buf, 735 * 2 + 4096);
    int tail = 0; for (int i = 2 * (n - 700); i < 2 * n; i++) tail |= buf[i];
    CHECK(tail == 0, "muted output is silent");
    /* ENDX: the looping block sets the voice's end flag */
    int ok = 1; uint32_t endx = io_read(x, 0x1F801D9C, 2, &ok);
    CHECK(ok && (endx & 1), "ENDX bit 0 set after the loop end (%04x)", endx);
    io_free(x);
}

static void test_xa_decode(void) {
    IoCtx *x = mk();
    uint8_t sec[2352]; memset(sec, 0, sizeof sec);
    sec[19] = 0x01;                                                  /* coding info: stereo, 37800 Hz, 4 bit */
    for (int g = 0; g < 0x12; g++) {
        uint8_t *p = sec + 24 + 128 * g;
        for (int k = 4; k < 12; k++) p[k] = 12;                       /* shift = 12 - 12 = 0, filter 0 */
        for (int k = 0; k < 28 * 4; k++) p[16 + k] = 0x32;           /* low nibble 2 (left), high nibble 3 (right) */
    }
    spu_xa_sector(x, sec);
    int total = (int)(x->spu.xa_w - x->spu.xa_r);
    CHECK(total == 18 * 4 * 28, "stereo sector gives %d frames, want %d", total, 18 * 4 * 28);
    CHECK(x->spu.xa[0][0] == 2 && x->spu.xa[1][0] == 3, "left 2 / right 3 with shift 0, got %d/%d", x->spu.xa[0][0], x->spu.xa[1][0]);
    CHECK(x->spu.xa[0][27] == 2 && x->spu.xa[1][27] == 3, "constant over the block (filter 0)");
    /* mono at 18900 Hz: every sample is doubled, both nibbles of a data word are consecutive halves of the block */
    x->spu.xa_w = x->spu.xa_r = 0;
    sec[19] = 0x04;
    spu_xa_sector(x, sec);
    total = (int)(x->spu.xa_w - x->spu.xa_r);
    CHECK(total == 18 * 4 * 56 * 2, "mono 18900 Hz sector gives %d frames, want %d", total, 18 * 4 * 56 * 2);
    CHECK(x->spu.xa[0][0] == 2 && x->spu.xa[0][1] == 2 && x->spu.xa[1][0] == 2, "mono duplicated to both channels and to two output frames");
    io_free(x);
}

static uint8_t bcd8(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void cd_cmd(IoCtx *x, int cmd, const uint8_t *par, int n) {
    w8(x, 0x1F801800, 0);
    for (int i = 0; i < n; i++) w8(x, 0x1F801802, par[i]);
    w8(x, 0x1F801801, (uint32_t)cmd);
}
static void test_cd(void) {
    IoCtx *x = mk();
    char path[512]; tmp_path(path, sizeof path, "cd");
    FILE *f = fopen(path, "wb"); CHECK(f != NULL, "temporary file");
    for (int s = 0; s < 40; s++) {                                    /* LBA s: sync, header (BCD mm:ss:ff of s+150), mode 2, form 1 data = byte pattern */
        uint8_t sec[2352]; memset(sec, 0, sizeof sec);
        memset(sec + 1, 0xFF, 10);
        int a = s + 150; sec[12] = bcd8(a / 4500); sec[13] = bcd8((a / 75) % 60); sec[14] = bcd8(a % 75); sec[15] = 2;
        for (int k = 0; k < 0x800; k++) sec[24 + k] = (uint8_t)(s * 7 + k);
        fwrite(sec, 1, sizeof sec, f);
    }
    fclose(f);
    CHECK(io_cd_open(x, path), "open image");
    w8(x, 0x1F801800, 1); w8(x, 0x1F801802, 0x1F); w8(x, 0x1F801803, 0x1F);   /* enable and ack all CD interrupts (index 1) */
    w32(x, 0x1F801074, 0xFFFFFFFF);
    uint8_t loc[3] = {0, 2, bcd8(10)};                                  /* 00:02:10 = LBA 10 */
    cd_cmd(x, 0x02, loc, 3);
    for (int i = 0; i < 3; i++) io_vblank_tick(x);
    w8(x, 0x1F801800, 1);
    CHECK((r8(x, 0x1F801803) & 7) == 3, "Setloc answers INT3");
    w8(x, 0x1F801803, 0x1F);
    uint8_t mode = 0x80; cd_cmd(x, 0x0E, &mode, 1);                     /* Setmode: double speed, 800h-byte sectors */
    for (int i = 0; i < 3; i++) io_vblank_tick(x);
    w8(x, 0x1F801800, 1); w8(x, 0x1F801803, 0x1F);
    cd_cmd(x, 0x06, NULL, 0);                                            /* ReadN */
    int got = 0;
    for (int i = 0; i < 40 && !got; i++) {
        io_vblank_tick(x);
        w8(x, 0x1F801800, 1);
        uint32_t fl = r8(x, 0x1F801803) & 7;
        if (fl == 1) got = 1;
        else if (fl) w8(x, 0x1F801803, 0x1F);                           /* acknowledge the INT3 answer of ReadN first */
    }
    CHECK(got, "ReadN delivers a sector (INT1)");
    w8(x, 0x1F801800, 0); w8(x, 0x1F801803, 0x80);                      /* request data: bit 7 of the request register (index 0) */
    int bad = 0; for (int k = 0; k < 16; k++) { uint32_t v = r8(x, 0x1F801802); if (v != (uint8_t)(10 * 7 + k)) bad++; }
    CHECK(bad == 0, "data FIFO returns the first bytes of LBA 10 (%d wrong)", bad);
    w8(x, 0x1F801803, 0x80);                                            /* BFRD again while the sector is partly read: the read position must stay (finding 0002) */
    uint32_t nxt = r8(x, 0x1F801802);
    CHECK(nxt == (uint8_t)(10 * 7 + 16), "a second BFRD does not rewind a partly read sector (read %02x, want %02x)", nxt, (uint8_t)(10 * 7 + 16));
    io_free(x); remove(path);
}

static void test_gpu(void) {
    IoCtx *x = mk();
    int ok = 1;
    io_write(x, 0x1F801814, 4, 0x08000000 | 0x01, &ok);                 /* GP1(08): 320x240, 15 bit (only to make the display rect sane) */
    io_write(x, 0x1F801810, 4, 0xE3000000, &ok);                        /* drawing area top-left (0,0) */
    io_write(x, 0x1F801810, 4, 0xE4000000 | (239 << 10) | 319, &ok);
    io_write(x, 0x1F801810, 4, 0xE5000000, &ok);
    io_write(x, 0x1F801810, 4, 0x02FF0000, &ok);                        /* fill 16x16 at (32,32) with blue (colour 0x00BBGGRR: B=FF) */
    io_write(x, 0x1F801810, 4, (32 << 16) | 32, &ok);
    io_write(x, 0x1F801810, 4, (16 << 16) | 16, &ok);
    uint16_t *v = io_vram(x);
    CHECK(v[32 * 1024 + 32] == (0x1F << 10) && v[47 * 1024 + 47] == (0x1F << 10) && v[48 * 1024 + 48] == 0, "fill rectangle covers exactly 16x16 (blue = 0x%04x)", v[32 * 1024 + 32]);
    /* flat monochrome triangle (28h-ish: 20h) red, vertices (100,100) (140,100) (100,140): inside point (110,110) red, outside (139,139) untouched */
    io_write(x, 0x1F801810, 4, 0x200000FF, &ok);
    io_write(x, 0x1F801810, 4, (100 << 16) | 100, &ok);
    io_write(x, 0x1F801810, 4, (100 << 16) | 140, &ok);
    io_write(x, 0x1F801810, 4, (140 << 16) | 100, &ok);
    CHECK(v[110 * 1024 + 110] == 0x1F, "inside the triangle is red (0x%04x)", v[110 * 1024 + 110]);
    CHECK(v[139 * 1024 + 139] == 0, "outside the triangle is untouched");
    /* lines: k+1 pixels along the major axis (horizontal line from (200,50) to (209,50): 10 pixels), poly-line segments between consecutive vertices */
    io_write(x, 0x1F801810, 4, 0x40FFFFFF, &ok); io_write(x, 0x1F801810, 4, (50 << 16) | 200, &ok); io_write(x, 0x1F801810, 4, (50 << 16) | 209, &ok);
    int cnt = 0; for (int xx = 190; xx < 220; xx++) cnt += v[50 * 1024 + xx] == 0x7FFF;
    CHECK(cnt == 10 && v[50 * 1024 + 200] == 0x7FFF && v[50 * 1024 + 209] == 0x7FFF && v[50 * 1024 + 210] == 0, "horizontal line covers exactly 10 pixels (%d)", cnt);
    io_write(x, 0x1F801810, 4, 0x48FF0000, &ok);                           /* flat poly-line (blue), three vertices, terminator */
    io_write(x, 0x1F801810, 4, (100 << 16) | 300, &ok); io_write(x, 0x1F801810, 4, (100 << 16) | 310, &ok); io_write(x, 0x1F801810, 4, (110 << 16) | 310, &ok);
    io_write(x, 0x1F801810, 4, 0x55555555, &ok);
    CHECK(v[100 * 1024 + 305] == (0x1F << 10) && v[105 * 1024 + 310] == (0x1F << 10), "poly-line draws both segments");
    /* GP0 NOPs 04h..1Eh and E0h are one word each (the game's ordering tables contain tag words as data) */
    io_write(x, 0x1F801810, 4, 0x0809A9B0, &ok);
    CHECK(ok && io_last_error(x)[0] == 0, "GP0 0x08 is a NOP");
    /* GPUSTAT bit 31 (odd/even field) alternates per frame in vertical-interlace mode and is 0 otherwise (a program busy-waits on it, finding 0037) */
    uint32_t st = io_read(x, 0x1F801814, 4, &ok);
    CHECK((st >> 31) == 0, "GPUSTAT bit 31 is 0 without interlace");
    io_write(x, 0x1F801814, 4, 0x08000000 | 0x24, &ok);                 /* GP1(08): vertical resolution 480 + vertical interlace */
    uint32_t a = io_read(x, 0x1F801814, 4, &ok) >> 31; io_vblank_tick(x);
    uint32_t b = io_read(x, 0x1F801814, 4, &ok) >> 31; io_vblank_tick(x);
    uint32_t c2 = io_read(x, 0x1F801814, 4, &ok) >> 31;
    CHECK(a != b && a == c2, "GPUSTAT bit 31 alternates every frame in interlace mode (%u %u %u)", a, b, c2);
    io_free(x);
}


static void test_gpu_hardware_semantics(void) {
    /* behaviours learned from the public ps1-tests captures (finding 0043) */
    IoCtx *x = mk();
    int ok = 1;
    uint16_t *v = io_vram(x);
    io_write(x, 0x1F801814, 4, 0x09000000, &ok);                        /* GP1(09h): texture disable not allowed */
    io_write(x, 0x1F801810, 4, 0xE1000FFF, &ok);
    CHECK((io_read(x, 0x1F801814, 4, &ok) & 0x87FF) == 0x07FF, "GPUSTAT bit 15 ignores E1 bit 11 when texture disable is not allowed");
    io_write(x, 0x1F801814, 4, 0x09000001, &ok);
    io_write(x, 0x1F801810, 4, 0xE1000FFF, &ok);
    CHECK((io_read(x, 0x1F801814, 4, &ok) & 0x87FF) == 0x87FF, "GPUSTAT bit 15 follows E1 bit 11 when it is allowed");
    /* VRAM -> VRAM copy: each row is read completely before it is written */
    for (int i = 0; i < 4; i++) v[i] = (uint16_t)(1 + i);
    io_write(x, 0x1F801810, 4, 0x80000000, &ok); io_write(x, 0x1F801810, 4, 0x00000000, &ok);
    io_write(x, 0x1F801810, 4, 0x00000001, &ok); io_write(x, 0x1F801810, 4, 0x00010004, &ok);          /* copy (0,0) -> (1,0), 4 x 1 */
    CHECK(v[1] == 1 && v[2] == 2 && v[3] == 3 && v[4] == 4, "overlapping VRAM copy keeps the row (%u %u %u %u)", v[1], v[2], v[3], v[4]);
    /* mask bit: E6.0 sets bit 15 of CPU->VRAM writes, E6.1 protects pixels that have it */
    io_write(x, 0x1F801810, 4, 0xE6000001, &ok);
    io_write(x, 0x1F801810, 4, 0xA0000000, &ok); io_write(x, 0x1F801810, 4, 0x00640064, &ok); io_write(x, 0x1F801810, 4, 0x00010001, &ok); io_write(x, 0x1F801810, 4, 0x00001234, &ok);
    CHECK(v[100 * 1024 + 100] == 0x9234, "E6.0 sets the mask bit on a CPU->VRAM write (0x%04x)", v[100 * 1024 + 100]);
    io_write(x, 0x1F801810, 4, 0xE6000002, &ok);
    io_write(x, 0x1F801810, 4, 0xA0000000, &ok); io_write(x, 0x1F801810, 4, 0x00640064, &ok); io_write(x, 0x1F801810, 4, 0x00010001, &ok); io_write(x, 0x1F801810, 4, 0x00000042, &ok);
    CHECK(v[100 * 1024 + 100] == 0x9234, "E6.1 protects a pixel whose mask bit is set (0x%04x)", v[100 * 1024 + 100]);
    io_free(x);
}

static void test_dma_register_semantics(void) {
    /* public ps1-tests dma/otc-test and spu/memory-transfer (finding 0043) */
    IoCtx *x = mk();
    int ok = 1;
    io_write(x, 0x1F8010E8, 4, 0x70770703, &ok);                          /* OTC control: bit 24 clear so nothing runs; only bits 28 and 30 stay, bit 1 reads 1 */
    CHECK(io_read(x, 0x1F8010E8, 4, &ok) == 0x50000002u, "OTC control keeps bits 30, 28 and the hard-wired bit 1 (0x%08x)", io_read(x, 0x1F8010E8, 4, &ok));
    io_write(x, 0x1F8010E8, 4, 0x8E88F8FC, &ok);
    CHECK(io_read(x, 0x1F8010E8, 4, &ok) == 0x00000002u, "OTC control unused bits read 0 (0x%08x)", io_read(x, 0x1F8010E8, 4, &ok));
    /* SPU DMA in start-immediately mode runs on the enable bit alone */
    io_write(x, 0x1F8010F0, 4, io_read(x, 0x1F8010F0, 4, &ok) | (1u << 19), &ok);   /* DPCR: channel 4 enabled (bit 19) */
    memcpy(g_ram + 0x1000, "ABCDEFGH", 8);
    io_write(x, 0x1F801DA6, 2, 0x0100, &ok);                              /* SPU transfer address 0x800 */
    io_write(x, 0x1F8010C0, 4, 0x1000, &ok); io_write(x, 0x1F8010C4, 4, 2, &ok);
    io_write(x, 0x1F8010C8, 4, 0x01000001, &ok);                          /* from RAM, sync 0, enable only */
    CHECK(memcmp(x->spuram + 0x800, "ABCDEFGH", 8) == 0, "SPU DMA in sync mode 0 transfers BCR words without the trigger bit");
    io_free(x);
}

static void test_dma_irq_deferred(void) {
    /* finding 0003: the completion interrupt of an instantaneous DMA must appear only when the host raises it, not inside the handler that started the transfer */
    IoCtx *x = mk();
    int ok = 1;
    w32(x, 0x1F8010F0, 0x0B000000);                                     /* DPCR: channel 6 enabled */
    w32(x, 0x1F8010F4, (1u << 23) | (1u << 22));                        /* DICR: master enable + channel 6 enable */
    w32(x, 0x1F801074, 0xFFFFFFFF);
    w32(x, 0x1F8010E0, 0x1000); w32(x, 0x1F8010E4, 4); w32(x, 0x1F8010E8, 0x11000002);
    CHECK((io_read(x, 0x1F801070, 4, &ok) & 8) == 0, "no DMA interrupt right after the transfer");
    CHECK(io_dma_irq_next(x) == 1, "the host raises the queued DMA interrupt");
    CHECK((io_read(x, 0x1F801070, 4, &ok) & 8) != 0 && ((io_read(x, 0x1F8010F4, 4, &ok) >> 30) & 1), "I_STAT bit 3 and the DICR channel-6 flag are set");
    CHECK(io_dma_irq_next(x) == 0, "nothing left");
    io_free(x);
}

static uint32_t sio_xfer(IoCtx *x, uint8_t tx) {
    int ok = 1;
    io_write(x, 0x1F801040, 1, tx, &ok);
    for (int k = 0; k < 4; k++) io_read(x, 0x1F801044, 2, &ok);        /* the answer appears a few STAT reads later (finding 0006) */
    return io_read(x, 0x1F801040, 1, &ok);
}
static void test_sio_pad(void) {
    /* psx-spx "Controllers": digital pad poll = 01h 42h 00h 00h 00h answered FFh 41h 5Ah <buttons low> <buttons high> (active low); cross is bit 14 */
    IoCtx *x = mk();
    int ok = 1;
    io_sio_set_pad(x, 0xFFFFFFFFu & ~(1u << 6));                         /* B(16h) format: cross pressed */
    io_write(x, 0x1F80104A, 2, 0x1003, &ok);                            /* port 1, ACK interrupt enabled, DTR, TXEN */
    uint32_t r1 = sio_xfer(x, 0x01), r2 = sio_xfer(x, 0x42), r3 = sio_xfer(x, 0), r4 = sio_xfer(x, 0), r5 = sio_xfer(x, 0);
    CHECK(r1 == 0xFF && r2 == 0x41 && r3 == 0x5A && r4 == 0xFF && r5 == 0xBF, "pad poll bytes %02x %02x %02x %02x %02x", r1, r2, r3, r4, r5);
    CHECK((io_read(x, 0x1F801070, 4, &ok) & 0x80) != 0, "the ACK sets I_STAT bit 7");
    io_write(x, 0x1F80104A, 2, 0x3003, &ok);                            /* port 2: nobody answers */
    CHECK(sio_xfer(x, 0x01) == 0xFF && x->sio.state == 0, "port 2 is empty");
    /* a program that spins on root counter 2 sees time pass inside a frame (finding 0007) */
    uint32_t a = io_read(x, 0x1F801120, 4, &ok), b = io_read(x, 0x1F801120, 4, &ok);
    CHECK(b > a, "root counter 2 advances between reads (%u -> %u)", a, b);
    io_free(x);
}

static void test_mdec_dma_stall(void) {
    /* finding 0010: MDEC->RAM waits for decoded data; RAM->MDEC waits until the output has been read (the decoder streams; nothing is dropped or invented) */
    IoCtx *x = mk();
    int ok = 1;
    w32(x, 0x1F8010F0, io_read(x, 0x1F8010F0, 4, &ok) | 0x000000FF);                  /* DPCR: channels enabled */
    x->md.outcap = 64; x->md.out = calloc(1, 64); x->md.nout = 16; x->md.outpos = 0;
    for (int k = 0; k < 16; k++) x->md.out[k] = (uint8_t)(k + 1);
    w32(x, 0x1F801090, 0x2000); w32(x, 0x1F801094, 0x00010008); w32(x, 0x1F801098, 0x01000200);     /* channel 1: 8 words from the decoder, only 4 are there */
    CHECK(x->d1_pend && x->d1_left == 4, "short output leaves the rest pending (%u words)", x->d1_left);
    CHECK((x->ch[1].chcr >> 24 & 1) && !(x->dma_pend & 2), "the transfer is still busy and has not raised its interrupt");
    CHECK(x->ram[0x2000] == 1 && x->ram[0x200F] == 16 && x->ram[0x2010] == 0, "the four words that existed arrived, nothing was invented");
    x->md.nout = 32; for (int k = 16; k < 32; k++) x->md.out[k] = (uint8_t)(k + 1);
    mdec_dma1_resume(x);
    CHECK(!x->d1_pend && !(x->ch[1].chcr >> 24 & 1) && (x->dma_pend & 2) && x->ram[0x201F] == 32, "more output completes the transfer");
    x->md.nout = 16; x->md.outpos = 0;                                                    /* output not read yet: new input must wait */
    w32(x, 0x1F801080, 0x3000); w32(x, 0x1F801084, 0x00010004); w32(x, 0x1F801088, 0x01000201);
    CHECK(x->d0_pend && (x->ch[0].chcr >> 24 & 1), "input DMA waits while output is unread");
    x->md.outpos = 16;
    mdec_dma0_resume(x);
    CHECK(!x->d0_pend && !(x->ch[0].chcr >> 24 & 1) && (x->dma_pend & 1), "input DMA runs once the output is read");
    /* an input transfer whose data has been consumed stays busy until the decoded output has been read, and the status register says busy meanwhile */
    x->ch[0].chcr |= 1u << 24; x->dma_pend = 0; x->d0_hold = 1; x->md.nout = 16; x->md.outpos = 4;
    CHECK((io_read(x, 0x1F801824, 4, &ok) >> 29) & 1, "MDEC status: busy while decoded output is unread");
    mdec_dma0_resume(x);
    CHECK(x->d0_hold && (x->ch[0].chcr >> 24 & 1) && !(x->dma_pend & 1), "input transfer is still busy while output remains");
    x->md.outpos = 16; mdec_dma0_resume(x);
    CHECK(!x->d0_hold && !(x->ch[0].chcr >> 24 & 1) && (x->dma_pend & 1) && !((io_read(x, 0x1F801824, 4, &ok) >> 29) & 1), "output read out: input transfer completes, status idle");
    free(x->md.out); x->md.out = NULL;
    io_free(x);
}

static void test_clut_cache(void) {
    /* the GPU caches the palette of 4-bit textures: VRAM writes do not refresh it, GP0(01h) empties it (public ps1-tests gpu/clut-cache, finding 0043) */
    IoCtx *x = mk();
    int ok = 1;
    uint16_t *v = io_vram(x);
    io_write(x, 0x1F801810, 4, 0xE3000000, &ok);
    io_write(x, 0x1F801810, 4, 0xE4000000 | (239 << 10) | 319, &ok);
    io_write(x, 0x1F801810, 4, 0xE1000000, &ok);                          /* texture page (0,0), 4-bit, no semi-transparency */
    v[0] = 0x0001;                                                        /* first texture word: texel 0 = palette index 1 */
    v[100 * 1024 + 1] = 0x001F;                                           /* palette at (0,100), entry 1 = red */
    #define SPRITE(row) do { io_write(x, 0x1F801810, 4, 0x65808080, &ok); io_write(x, 0x1F801810, 4, ((uint32_t)(row) << 16) | 40u, &ok); io_write(x, 0x1F801810, 4, (100u << 22) | 0u, &ok); io_write(x, 0x1F801810, 4, 0x00010001, &ok); } while (0)
    SPRITE(50);
    CHECK(v[50 * 1024 + 40] == 0x001F, "a 4-bit textured sprite takes its palette colour (0x%04x)", v[50 * 1024 + 40]);
    v[100 * 1024 + 1] = 0x03E0;                                           /* the palette in VRAM changes without telling the GPU */
    SPRITE(51);
    CHECK(v[51 * 1024 + 40] == 0x001F, "the cached palette is still used after a VRAM write (0x%04x)", v[51 * 1024 + 40]);
    io_write(x, 0x1F801810, 4, 0x01000000, &ok);                           /* GP0(01h): clear texture cache */
    SPRITE(52);
    CHECK(v[52 * 1024 + 40] == 0x03E0, "after GP0(01h) the new palette is read (0x%04x)", v[52 * 1024 + 40]);
    #undef SPRITE
    io_free(x);
}

static void test_timer2(void) {
    IoCtx *x = mk();
    int ok = 1;
    /* root counter 2 as a sound engine programs it: sysclk/8, target 0xA560, reset + IRQ on target, repeat -> 100 Hz (finding 0040) */
    io_write(x, 0x1F801128, 4, 0, &ok);
    io_write(x, 0x1F801124, 4, 0x258, &ok);
    io_write(x, 0x1F801128, 4, 0xA560, &ok);
    int irqs = 0;
    for (int f = 0; f < 60; f++) { io_vblank_tick(x); while (io_timer_irq_next(x)) { irqs++; io_write(x, 0x1F801070, 4, ~(1u << 6), &ok); } }
    CHECK(irqs >= 99 && irqs <= 101, "timer 2 at target 0xA560, sysclk/8 raises 100 interrupts per second (%d)", irqs);
    io_write(x, 0x1F801124, 4, 0x258 & ~0x10, &ok);                     /* IRQ on target off */
    irqs = 0;
    for (int f = 0; f < 10; f++) { io_vblank_tick(x); while (io_timer_irq_next(x)) irqs++; }
    CHECK(irqs == 0, "no interrupts without the IRQ-on-target bit");
    io_write(x, 0x1F801124, 4, 0x218, &ok);                             /* IRQ on target, one shot (bit 6 clear) */
    irqs = 0;
    for (int f = 0; f < 10; f++) { io_vblank_tick(x); while (io_timer_irq_next(x)) irqs++; }
    CHECK(irqs == 1, "one-shot mode interrupts once (%d)", irqs);
    io_free(x);
}

static void test_mdec(void) {
    IoCtx *x = mk();
    int ok = 1;
    static const uint16_t sc[64] = {                       /* psx-spx "MDEC(3) scale table" (row = frequency) */
        0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82, 0x7D8A,0x6A6D,0x471C,0x18F8,0xE707,0xB8E3,0x9592,0x8275,
        0x7641,0x30FB,0xCF04,0x89BE,0x89BE,0xCF04,0x30FB,0x7641, 0x6A6D,0xE707,0x8275,0xB8E3,0x471C,0x7D8A,0x18F8,0x9592,
        0x5A82,0xA57D,0xA57D,0x5A82,0x5A82,0xA57D,0xA57D,0x5A82, 0x471C,0x8275,0x18F8,0x6A6D,0x9592,0xE707,0x7D8A,0xB8E3,
        0x30FB,0x89BE,0x7641,0xCF04,0xCF04,0x7641,0x89BE,0x30FB, 0x18F8,0xB8E3,0x6A6D,0x8275,0x7D8A,0x9592,0x471C,0xE707};
    io_write(x, 0x1F801820, 4, 0x40000001, &ok);                       /* set quant tables: all 2 */
    for (int i = 0; i < 32; i++) io_write(x, 0x1F801820, 4, 0x02020202, &ok);
    io_write(x, 0x1F801820, 4, 0x60000000, &ok);
    for (int i = 0; i < 32; i++) io_write(x, 0x1F801820, 4, (uint32_t)sc[2 * i] | ((uint32_t)sc[2 * i + 1] << 16), &ok);
    /* one 24 bit macroblock: Cr = Cb = 0, four Y blocks with DC 50 (quant scale 1): coefficient 50*2 = 100, DC-only IDCT = 100*0x5A82^2/2^32 = 12.499 -> 12 (+128 = 140 = 0x8C) */
    uint16_t hw[12];
    for (int b = 0; b < 6; b++) { hw[2 * b] = (uint16_t)((1 << 10) | (b < 2 ? 0 : 50)); hw[2 * b + 1] = 0xFE00; }
    io_write(x, 0x1F801820, 4, 0x30000000u | 6, &ok);
    for (int i = 0; i < 6; i++) io_write(x, 0x1F801820, 4, (uint32_t)hw[2 * i] | ((uint32_t)hw[2 * i + 1] << 16), &ok);
    int bad = 0;
    for (int i = 0; i < 192; i++) { uint32_t w = io_read(x, 0x1F801820, 4, &ok); for (int k = 0; k < 4; k++) if (((w >> (8 * k)) & 0xFF) != 0x8C) bad++; }
    CHECK(bad == 0, "DC-only luminance block decodes to a constant 0x8C grey (%d pixels differ)", bad);
    io_free(x);
}

int main(void) {
    test_adpcm_and_adsr(); test_adsr_release(); test_spu_output(); test_xa_decode(); test_mdec(); test_cd(); test_gpu(); test_gpu_hardware_semantics(); test_dma_register_semantics(); test_dma_irq_deferred(); test_sio_pad(); test_mdec_dma_stall(); test_clut_cache(); test_timer2();
    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
