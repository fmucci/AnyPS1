/* GTE (COP2) for the PS1 (queue P4-1): register file semantics and every command the executables use (finding 0009) plus the rest of the
 * psx-spx command list. Written from psx-spx "GTE Registers / Saturation / Opcode Summary / Coordinate, General Purpose and Color Calculation
 * Commands / Division Inaccuracy"; the points psx-spx leaves open (overflow checks on partial sums, the far-colour MVMVA bug, the exact
 * order of saturations) are fixed by comparison with an independent implementation (tests/gte/, finding 0024).
 * Registers: d[0..31] = cop2r0..31, c[0..31] = cop2r32..63. */
#include <stddef.h>
#include "gte.h"

/* ---- register views ---- */
#define S16(x) ((int16_t)(x))
#define VX(n) S16(d[2 * (n)])
#define VY(n) S16(d[2 * (n)] >> 16)
#define VZ(n) S16(d[2 * (n) + 1])
#define IR0 S16(d[8])
#define IR(i) S16(d[8 + (i)])          /* i = 1..3 */
#define MAC(i) ((int32_t)d[24 + (i)])  /* i = 0..3 */
#define RGBC_R (d[6] & 0xFF)
#define RGBC_G ((d[6] >> 8) & 0xFF)
#define RGBC_B ((d[6] >> 16) & 0xFF)
#define RGBC_CODE (d[6] >> 24)
#define SX(n) S16(d[12 + (n)])
#define SY(n) S16(d[12 + (n)] >> 16)
#define SZ(n) ((int32_t)(d[16 + (n)] & 0xFFFF))
#define FLAG c[31]

static int32_t mat(const uint32_t *c, int base, int i, int j) {     /* 3x3 s16 matrix packed two per word, row major */
    int k = i * 3 + j;
    return S16(c[base + k / 2] >> ((k & 1) * 16));
}

/* ---- saturation and flags ---- */
#define MAX44 ((int64_t)0x7FFFFFFFFFFLL)
#define MIN44 (-(int64_t)0x80000000000LL)

static int64_t sext44(int64_t v) { return (int64_t)((uint64_t)v << 20) >> 20; }

/* overflow check of MACi (i = 1..3) on a 44-bit intermediate; sets the flag and returns the 44-bit wrapped value */
static int64_t chk(uint32_t *c, int i, int64_t v) {
    if (v > MAX44) { FLAG |= 1u << (31 - i); return sext44(v); }
    if (v < MIN44) { FLAG |= 1u << (28 - i); return sext44(v); }
    return v;
}
static void mac0(uint32_t *c, uint32_t *d, int64_t v) {
    if (v > 0x7FFFFFFFLL) FLAG |= 1u << 16;
    else if (v < -0x80000000LL) FLAG |= 1u << 15;
    d[24] = (uint32_t)v;
}
/* store MACi = v >> shift (after the overflow check on v) and return it */
static int32_t setmac(uint32_t *c, uint32_t *d, int i, int64_t v, int shift) {
    if (v > MAX44) FLAG |= 1u << (31 - i);
    else if (v < MIN44) FLAG |= 1u << (28 - i);
    int32_t m = (int32_t)(v >> shift);
    d[24 + i] = (uint32_t)m;
    return m;
}
static void setir(uint32_t *c, uint32_t *d, int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { v = lo; FLAG |= 1u << (25 - i); }
    else if (v > 0x7FFF) { v = 0x7FFF; FLAG |= 1u << (25 - i); }
    d[8 + i] = (uint32_t)v;
}
static void setir0(uint32_t *c, uint32_t *d, int32_t v) {
    if (v < 0) { v = 0; FLAG |= 1u << 12; }
    else if (v > 0x1000) { v = 0x1000; FLAG |= 1u << 12; }
    d[8] = (uint32_t)v;
}
static void macir(uint32_t *c, uint32_t *d, int i, int64_t v, int shift, int lm) { setir(c, d, i, setmac(c, d, i, v, shift), lm); }

static void push_sz(uint32_t *c, uint32_t *d, int32_t v) {
    if (v < 0) { v = 0; FLAG |= 1u << 18; } else if (v > 0xFFFF) { v = 0xFFFF; FLAG |= 1u << 18; }
    d[16] = d[17]; d[17] = d[18]; d[18] = d[19]; d[19] = (uint32_t)v;
}
static void push_sxy(uint32_t *c, uint32_t *d, int32_t x, int32_t y) {
    if (x < -0x400) { x = -0x400; FLAG |= 1u << 14; } else if (x > 0x3FF) { x = 0x3FF; FLAG |= 1u << 14; }
    if (y < -0x400) { y = -0x400; FLAG |= 1u << 13; } else if (y > 0x3FF) { y = 0x3FF; FLAG |= 1u << 13; }
    d[12] = d[13]; d[13] = d[14]; d[14] = ((uint32_t)x & 0xFFFFu) | ((uint32_t)y << 16);
}
static uint32_t sat8(uint32_t *c, int bit, int32_t v) {
    if (v < 0) { FLAG |= 1u << bit; return 0; }
    if (v > 0xFF) { FLAG |= 1u << bit; return 0xFF; }
    return (uint32_t)v;
}
static void push_color(uint32_t *c, uint32_t *d) {
    uint32_t r = sat8(c, 21, MAC(1) >> 4), g = sat8(c, 20, MAC(2) >> 4), b = sat8(c, 19, MAC(3) >> 4);
    d[20] = d[21]; d[21] = d[22]; d[22] = r | (g << 8) | (b << 16) | (RGBC_CODE << 24);
}

/* ---- division (psx-spx "GTE Division Inaccuracy"): n = min(1FFFFh, ((H * 20000h / SZ3) + 1) / 2) computed with the UNR table ---- */
static uint8_t unr_table[257];
static int unr_ready;
static void unr_init(void) {
    for (int i = 0; i <= 256; i++) { int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101; unr_table[i] = (uint8_t)(v < 0 ? 0 : v); }
    unr_ready = 1;
}
static uint32_t unr_divide(uint32_t *c, uint32_t h, uint32_t z) {
    if (!unr_ready) unr_init();
    if (z * 2 <= h) { FLAG |= 1u << 17; return 0x1FFFF; }            /* H >= 2*SZ3 (also SZ3 = 0): divide overflow */
    int sh = 0;
    for (uint32_t t = z & 0xFFFF; sh < 16 && !(t & 0x8000); t <<= 1) sh++;   /* leading zeros of the 16-bit divisor */
    uint32_t n = h << sh, dv = z << sh;
    uint32_t div = dv | 0x8000;
    int32_t x = 0x101 + unr_table[((div & 0x7FFF) + 0x40) >> 7];
    int32_t dd = (int32_t)(((int64_t)(int32_t)div * -x + 0x80) >> 8);
    uint32_t recip = (uint32_t)(((int64_t)x * (0x20000 + dd) + 0x80) >> 8);
    uint64_t res = ((uint64_t)n * recip + 0x8000) >> 16;
    return res > 0x1FFFF ? 0x1FFFF : (uint32_t)res;
}

/* ---- command bodies; `sf` selects the shift, `lm` the IR lower limit ---- */
static void rtps(uint32_t *d, uint32_t *c, int v, int shift, int lm, int last) {
    int64_t t[3] = {(int32_t)c[5], (int32_t)c[6], (int32_t)c[7]};
    int64_t m[3];
    for (int i = 0; i < 3; i++) {
        int64_t a = chk(c, i + 1, (t[i] << 12) + (int64_t)mat(c, 0, i, 0) * VX(v));
        a = chk(c, i + 1, a + (int64_t)mat(c, 0, i, 1) * VY(v));
        m[i] = a + (int64_t)mat(c, 0, i, 2) * VZ(v);
    }
    int32_t m1 = setmac(c, d, 1, m[0], shift), m2 = setmac(c, d, 2, m[1], shift);
    setmac(c, d, 3, m[2], shift);
    setir(c, d, 1, m1, lm);
    setir(c, d, 2, m2, lm);
    /* IR3 flag from MAC3 >> 12 whatever sf is (psx-spx), the stored value saturates MAC3 itself */
    { int32_t z12 = (int32_t)(m[2] >> 12); if (z12 < -0x8000 || z12 > 0x7FFF) FLAG |= 1u << 22; }
    { int32_t v3 = MAC(3), lo = lm ? 0 : -0x8000; d[11] = (uint32_t)(v3 < lo ? lo : v3 > 0x7FFF ? 0x7FFF : v3); }
    push_sz(c, d, (int32_t)(m[2] >> 12));
    int64_t q = unr_divide(c, c[26] & 0xFFFF, d[19] & 0xFFFF);
    int64_t sx = q * IR(1) + (int32_t)c[24], sy = q * IR(2) + (int32_t)c[25];
    if (sx > 0x7FFFFFFFLL) FLAG |= 1u << 16; else if (sx < -0x80000000LL) FLAG |= 1u << 15;
    if (sy > 0x7FFFFFFFLL) FLAG |= 1u << 16; else if (sy < -0x80000000LL) FLAG |= 1u << 15;
    push_sxy(c, d, (int32_t)(sx >> 16), (int32_t)(sy >> 16));
    if (last) {
        int64_t sz = q * S16(c[27]) + (int32_t)c[28];
        mac0(c, d, sz);
        setir0(c, d, (int32_t)(sz >> 12));
    }
}

/* result = (T << 12 + M*V) with the 44-bit checks after each partial sum; t == NULL means no translation */
static void matvec(uint32_t *d, uint32_t *c, int mbase, const int32_t *t, int vx, int vy, int vz, int shift, int lm) {
    for (int i = 0; i < 3; i++) {
        int64_t a;
        if (t) {
            a = chk(c, i + 1, ((int64_t)t[i] << 12) + (int64_t)mat(c, mbase, i, 0) * vx);
            a = chk(c, i + 1, a + (int64_t)mat(c, mbase, i, 1) * vy);
        } else {
            a = chk(c, i + 1, (int64_t)mat(c, mbase, i, 0) * vx + (int64_t)mat(c, mbase, i, 1) * vy);
        }
        macir(c, d, i + 1, a + (int64_t)mat(c, mbase, i, 2) * vz, shift, lm);
    }
}

static void interpolate(uint32_t *d, uint32_t *c, int64_t a1, int64_t a2, int64_t a3, int shift, int lm) {
    macir(c, d, 1, ((int64_t)(int32_t)c[21] << 12) - a1, shift, 0);
    macir(c, d, 2, ((int64_t)(int32_t)c[22] << 12) - a2, shift, 0);
    macir(c, d, 3, ((int64_t)(int32_t)c[23] << 12) - a3, shift, 0);
    macir(c, d, 1, (int64_t)IR(1) * IR0 + a1, shift, lm);
    macir(c, d, 2, (int64_t)IR(2) * IR0 + a2, shift, lm);
    macir(c, d, 3, (int64_t)IR(3) * IR0 + a3, shift, lm);
}

static void light_color(uint32_t *d, uint32_t *c, int vx, int vy, int vz, int shift, int lm) {
    int32_t bk[3] = {(int32_t)c[13], (int32_t)c[14], (int32_t)c[15]};
    matvec(d, c, 8, NULL, vx, vy, vz, shift, lm);                                     /* LLM * V */
    matvec(d, c, 16, bk, IR(1), IR(2), IR(3), shift, lm);                              /* BK*1000h + LCM * IR */
}
static void mul_rgb(uint32_t *d, uint32_t *c, int shift, int lm) {                     /* [R*IR1, G*IR2, B*IR3] << 4 */
    macir(c, d, 1, (int64_t)((int32_t)RGBC_R * IR(1)) << 4, shift, lm);
    macir(c, d, 2, (int64_t)((int32_t)RGBC_G * IR(2)) << 4, shift, lm);
    macir(c, d, 3, (int64_t)((int32_t)RGBC_B * IR(3)) << 4, shift, lm);
}

static void ncs(uint32_t *d, uint32_t *c, int v, int sh, int lm) { light_color(d, c, VX(v), VY(v), VZ(v), sh, lm); push_color(c, d); }
static void nccs(uint32_t *d, uint32_t *c, int v, int sh, int lm) { light_color(d, c, VX(v), VY(v), VZ(v), sh, lm); mul_rgb(d, c, sh, lm); push_color(c, d); }
static void ncds(uint32_t *d, uint32_t *c, int v, int sh, int lm) {
    light_color(d, c, VX(v), VY(v), VZ(v), sh, lm);
    int32_t a1 = (int32_t)((int32_t)RGBC_R * IR(1)) << 4, a2 = (int32_t)((int32_t)RGBC_G * IR(2)) << 4, a3 = (int32_t)((int32_t)RGBC_B * IR(3)) << 4;
    interpolate(d, c, a1, a2, a3, sh, lm);
    push_color(c, d);
}
static void dpcs(uint32_t *d, uint32_t *c, const uint32_t rgb[3], int sh, int lm) {
    setmac(c, d, 1, (int64_t)rgb[0] << 16, 0);
    setmac(c, d, 2, (int64_t)rgb[1] << 16, 0);
    setmac(c, d, 3, (int64_t)rgb[2] << 16, 0);
    interpolate(d, c, MAC(1), MAC(2), MAC(3), sh, lm);
    push_color(c, d);
}

/* returns 1 if the command is implemented */
int gte_core_cmd(uint32_t *d, uint32_t *c, uint32_t w) {
    int sf = (w >> 19) & 1, lm = (w >> 10) & 1, sh = sf ? 12 : 0;
    FLAG = 0;
    switch (w & 0x3F) {
    case 0x01: rtps(d, c, 0, sh, lm, 1); break;
    case 0x30: rtps(d, c, 0, sh, lm, 0); rtps(d, c, 1, sh, lm, 0); rtps(d, c, 2, sh, lm, 1); break;
    case 0x06: {
        int64_t v = (int64_t)SX(0) * (SY(1) - SY(2)) + (int64_t)SX(1) * (SY(2) - SY(0)) + (int64_t)SX(2) * (SY(0) - SY(1));
        mac0(c, d, v);
        break;
    }
    case 0x2D: case 0x2E: {
        int64_t sum = (w & 0x3F) == 0x2D ? (int64_t)(SZ(1) + SZ(2) + SZ(3)) * S16(c[29]) : (int64_t)(SZ(0) + SZ(1) + SZ(2) + SZ(3)) * S16(c[30]);
        mac0(c, d, sum);
        int32_t o = (int32_t)(sum >> 12);
        if (o < 0) { o = 0; FLAG |= 1u << 18; } else if (o > 0xFFFF) { o = 0xFFFF; FLAG |= 1u << 18; }
        d[7] = (uint32_t)o;
        break;
    }
    case 0x12: {                                                          /* MVMVA */
        int mx = (w >> 17) & 3, v = (w >> 15) & 3, cv = (w >> 13) & 3;
        int vx, vy, vz;
        if (v == 3) { vx = IR(1); vy = IR(2); vz = IR(3); } else { vx = VX(v); vy = VY(v); vz = VZ(v); }
        int32_t zero[3] = {0, 0, 0};
        const int32_t tr[3] = {(int32_t)c[5], (int32_t)c[6], (int32_t)c[7]}, bk[3] = {(int32_t)c[13], (int32_t)c[14], (int32_t)c[15]};
        const int32_t fc[3] = {(int32_t)c[21], (int32_t)c[22], (int32_t)c[23]};
        const int32_t *t = cv == 0 ? tr : cv == 1 ? bk : cv == 2 ? fc : zero;
        uint32_t tmp[5];
        int mbase = mx == 0 ? 0 : mx == 1 ? 8 : 16;
        uint32_t *cm = c;
        if (mx == 3) {                                                    /* garbage matrix (psx-spx): -R*10h, +R*10h, IR0, RT13, RT13.., RT22.. */
            int32_t r = (int32_t)RGBC_R << 4;
            int16_t e[9] = {(int16_t)-r, (int16_t)r, (int16_t)IR0, (int16_t)mat(c, 0, 0, 2), (int16_t)mat(c, 0, 0, 2), (int16_t)mat(c, 0, 0, 2),
                            (int16_t)mat(c, 0, 1, 1), (int16_t)mat(c, 0, 1, 1), (int16_t)mat(c, 0, 1, 1)};
            for (int k = 0; k < 4; k++) tmp[k] = (uint16_t)e[2 * k] | ((uint32_t)(uint16_t)e[2 * k + 1] << 16);
            tmp[4] = (uint16_t)e[8];
            cm = tmp; mbase = 0;
            /* FLAG lives in c[31]: keep using the real control array for flags */
            uint32_t save[5]; for (int k = 0; k < 5; k++) save[k] = c[k];
            for (int k = 0; k < 5; k++) c[k] = tmp[k];
            if (cv != 2) matvec(d, c, 0, t, vx, vy, vz, sh, lm);
            else {
                for (int i = 0; i < 3; i++) {
                    int64_t a = chk(c, i + 1, ((int64_t)t[i] << 12) + (int64_t)mat(c, 0, i, 0) * vx);
                    setir(c, d, i + 1, (int32_t)(a >> sh), 0);
                    macir(c, d, i + 1, chk(c, i + 1, (int64_t)mat(c, 0, i, 1) * vy) + (int64_t)mat(c, 0, i, 2) * vz, sh, lm);
                }
            }
            for (int k = 0; k < 5; k++) c[k] = save[k];
            (void)cm;
        } else if (cv != 2) matvec(d, c, mbase, t, vx, vy, vz, sh, lm);
        else {                                                            /* far-colour bug (psx-spx): part 1 only affects IR and flags */
            for (int i = 0; i < 3; i++) {
                int64_t a = chk(c, i + 1, chk(c, i + 1, ((int64_t)t[i] << 12) + (int64_t)mat(c, mbase, i, 0) * vx));
                setir(c, d, i + 1, (int32_t)(a >> sh), 0);
                macir(c, d, i + 1, chk(c, i + 1, (int64_t)mat(c, mbase, i, 1) * vy) + (int64_t)mat(c, mbase, i, 2) * vz, sh, lm);
            }
        }
        break;
    }
    case 0x28: {                                                          /* SQR */
        for (int i = 1; i <= 3; i++) { int32_t m = (IR(i) * IR(i)) >> sh; d[24 + i] = (uint32_t)m; }
        for (int i = 1; i <= 3; i++) setir(c, d, i, MAC(i), lm);
        break;
    }
    case 0x0C: {                                                          /* OP */
        int32_t d1 = mat(c, 0, 0, 0), d2 = mat(c, 0, 1, 1), d3 = mat(c, 0, 2, 2), i1 = IR(1), i2 = IR(2), i3 = IR(3);
        macir(c, d, 1, (int64_t)(i3 * d2) - (int64_t)(i2 * d3), sh, lm);
        macir(c, d, 2, (int64_t)(i1 * d3) - (int64_t)(i3 * d1), sh, lm);
        macir(c, d, 3, (int64_t)(i2 * d1) - (int64_t)(i1 * d2), sh, lm);
        break;
    }
    case 0x1E: ncs(d, c, 0, sh, lm); break;
    case 0x20: ncs(d, c, 0, sh, lm); ncs(d, c, 1, sh, lm); ncs(d, c, 2, sh, lm); break;
    case 0x1B: nccs(d, c, 0, sh, lm); break;
    case 0x3F: nccs(d, c, 0, sh, lm); nccs(d, c, 1, sh, lm); nccs(d, c, 2, sh, lm); break;
    case 0x13: ncds(d, c, 0, sh, lm); break;
    case 0x16: ncds(d, c, 0, sh, lm); ncds(d, c, 1, sh, lm); ncds(d, c, 2, sh, lm); break;
    case 0x1C: {                                                          /* CC */
        int32_t bk[3] = {(int32_t)c[13], (int32_t)c[14], (int32_t)c[15]};
        matvec(d, c, 16, bk, IR(1), IR(2), IR(3), sh, lm);
        mul_rgb(d, c, sh, lm);
        push_color(c, d);
        break;
    }
    case 0x14: {                                                          /* CDP */
        int32_t bk[3] = {(int32_t)c[13], (int32_t)c[14], (int32_t)c[15]};
        matvec(d, c, 16, bk, IR(1), IR(2), IR(3), sh, lm);
        int32_t a1 = (int32_t)((int32_t)RGBC_R * IR(1)) << 4, a2 = (int32_t)((int32_t)RGBC_G * IR(2)) << 4, a3 = (int32_t)((int32_t)RGBC_B * IR(3)) << 4;
        interpolate(d, c, a1, a2, a3, sh, lm);
        push_color(c, d);
        break;
    }
    case 0x10: { uint32_t rgb[3] = {RGBC_R, RGBC_G, RGBC_B}; dpcs(d, c, rgb, sh, lm); break; }
    case 0x2A: for (int k = 0; k < 3; k++) { uint32_t rgb[3] = {d[20] & 0xFF, (d[20] >> 8) & 0xFF, (d[20] >> 16) & 0xFF}; dpcs(d, c, rgb, sh, lm); } break;
    case 0x29: {                                                          /* DCPL */
        int32_t a1 = (int32_t)((int32_t)RGBC_R * IR(1)) << 4, a2 = (int32_t)((int32_t)RGBC_G * IR(2)) << 4, a3 = (int32_t)((int32_t)RGBC_B * IR(3)) << 4;
        interpolate(d, c, a1, a2, a3, sh, lm);
        push_color(c, d);
        break;
    }
    case 0x11:                                                            /* INTPL */
        interpolate(d, c, (int64_t)(int32_t)IR(1) << 12, (int64_t)(int32_t)IR(2) << 12, (int64_t)(int32_t)IR(3) << 12, sh, lm);
        push_color(c, d);
        break;
    case 0x3E:                                                            /* GPL */
        macir(c, d, 1, (int64_t)IR(1) * IR0 + ((int64_t)MAC(1) << sh), sh, lm);
        macir(c, d, 2, (int64_t)IR(2) * IR0 + ((int64_t)MAC(2) << sh), sh, lm);
        macir(c, d, 3, (int64_t)IR(3) * IR0 + ((int64_t)MAC(3) << sh), sh, lm);
        push_color(c, d);
        break;
    case 0x3D:                                                            /* GPF */
        macir(c, d, 1, (int64_t)IR(1) * IR0, sh, lm);
        macir(c, d, 2, (int64_t)IR(2) * IR0, sh, lm);
        macir(c, d, 3, (int64_t)IR(3) * IR0, sh, lm);
        push_color(c, d);
        break;
    default: return 0;
    }
    if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u;
    return 1;
}

/* ---- register moves ---- */
uint32_t gte_core_mfc2(uint32_t *d, uint32_t *c, unsigned n) {
    (void)c;
    switch (n) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)S16(d[n]);      /* VZ0..VZ2, IR0..IR3: signed 16 bit */
    case 7: case 16: case 17: case 18: case 19: return d[n] & 0xFFFFu;                                  /* OTZ, SZ0..SZ3: unsigned 16 bit */
    case 15: return d[14];                                                                              /* SXYP reads SXY2 */
    case 28: case 29: {                                                                                 /* IRGB/ORGB: IR1..3 / 80h saturated to 0..1Fh */
        int32_t v[3] = {IR(1) >> 7, IR(2) >> 7, IR(3) >> 7};
        for (int i = 0; i < 3; i++) v[i] = v[i] < 0 ? 0 : v[i] > 0x1F ? 0x1F : v[i];
        return (uint32_t)(v[0] | (v[1] << 5) | (v[2] << 10));
    }
    case 31: {                                                                                          /* LZCR */
        uint32_t v = d[30], top = v >> 31, cnt = 0;
        for (int i = 31; i >= 0 && ((v >> i) & 1) == top; i--) cnt++;
        return cnt;
    }
    default: return d[n];
    }
}

uint32_t gte_core_cfc2(uint32_t *d, uint32_t *c, unsigned n) {
    (void)d;
    switch (n) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: return (uint32_t)(int32_t)S16(c[n]);    /* RT33, L33, LB33, H (hardware bug), DQA, ZSF3, ZSF4 */
    case 31: return (c[31] & 0x7FFFF000u) | ((c[31] & 0x7F87E000u) ? 0x80000000u : 0);
    default: return c[n];
    }
}

void gte_core_mtc2(uint32_t *d, uint32_t *c, unsigned n, uint32_t v) {
    (void)c;
    switch (n) {
    case 15: d[12] = d[13]; d[13] = d[14]; d[14] = v; break;                                             /* SXYP: push */
    case 28: d[28] = v & 0x7FFF; d[9] = (v & 0x1F) * 0x80; d[10] = ((v >> 5) & 0x1F) * 0x80; d[11] = ((v >> 10) & 0x1F) * 0x80; break;
    case 29: case 31: break;                                                                              /* read-only */
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: d[n] = (uint32_t)(int32_t)S16(v); break;
    case 7: case 16: case 17: case 18: case 19: d[n] = v & 0xFFFFu; break;
    default: d[n] = v; break;
    }
}

void gte_core_ctc2(uint32_t *d, uint32_t *c, unsigned n, uint32_t v) {
    (void)d;
    switch (n) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: c[n] = (uint32_t)(int32_t)S16(v); break;
    case 31: c[31] = v & 0x7FFFF000u; if (c[31] & 0x7F87E000u) c[31] |= 0x80000000u; break;
    default: c[n] = v; break;
    }
}
