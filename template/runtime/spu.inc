#include "spu_gauss.inc"
/* SPU model (queue P4-3), included by io.c. Sources: psx-spx "SPU ADPCM Samples / ADPCM Pitch / Volume and ADSR Generator / Voice Flags / Noise
 * Generator / Control and Status Register / Reverb Registers / Reverb Formula" and "CDROM XA Audio ADPCM Compression". Samples are generated at
 * 44100 Hz, 735 per virtual frame (io_vblank_tick), into a ring buffer that the host drains with io_spu_pull; the output also feeds a hash so the
 * differential tests compare audio exactly. SPU IRQ: raised (I_STAT bit 9) when a voice's ADPCM block fetch covers the IRQ address (finding 0019). Not modelled: voice volume sweep (applied as an immediate target), capture buffers,
 * the 25-point XA resampling filter (linear interpolation instead). */

static int s16c(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static const int adpcm_f0[5] = {0, 60, 115, 98, 122}, adpcm_f1[5] = {0, 0, -52, -55, -60};

/* SPU IRQ (registers 1F801DA4 = address / 8, SPUCNT bit 6 = enable, SPUSTAT bit 6 = flag; writing the enable bit as 0 acknowledges): the movie sound streams of the game use it to learn that a
 * half of the ring buffer was played (finding 0019). The hardware raises it when the SPU reads the 8-byte word at that address: here, when a voice fetches the 16-byte block holding it. */
static void spu_irq_check(IoCtx *x, uint32_t blk) {
    uint16_t cnt = x->spu_r[(0x1F801DAA - 0x1F801C00) >> 1];
    if (!(cnt & 0x40) || (x->spu_irqflag)) return;
    uint32_t a = ((uint32_t)x->spu_r[(0x1F801DA4 - 0x1F801C00) >> 1] * 8) & 0x7FFF8;
    if (a >= (blk & 0x7FFF0) && a < (blk & 0x7FFF0) + 16) { x->spu_irqflag = 1; x->i_stat |= 1u << 9; if (getenv("PS1_SPUIRQ")) fprintf(stderr, "[spuirq] frame %llu raised at %05x (i_mask %04x)\n", (unsigned long long)x->frames, (unsigned)a, (unsigned)x->i_mask); }
}

static void spu_decode_block(IoCtx *x, Voice *v) {
    spu_irq_check(x, v->cur);
    const uint8_t *b = x->spuram + (v->cur & 0x7FFF0);
    int shift = b[0] & 0xF, filter = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filter > 4) filter = 4;
    v->blk_flags = b[1];
    if (v->blk_flags & 4) { v->rep = v->cur & 0x7FFF0; v->rep_valid = 1; }                      /* loop start */
    v->s[0] = v->s[28]; v->s[1] = v->s[29]; v->s[2] = v->s[30];           /* keep the last 3 samples for interpolation */
    for (int j = 0; j < 28; j++) {
        int nib = (b[2 + j / 2] >> ((j & 1) * 4)) & 0xF;
        int t = nib >= 8 ? nib - 16 : nib;
        int smp = ((t * 4096) >> shift) + ((v->old * adpcm_f0[filter] + v->older * adpcm_f1[filter] + 32) >> 6);
        smp = s16c(smp);
        v->s[3 + j] = smp; v->older = v->old; v->old = smp;
    }
}

static void spu_keyon(IoCtx *x, int n) {
    Voice *v = &x->spu.v[n];
    v->on = 1; v->phase = 0; v->level = 0; v->adsr_cnt = 0;
    v->cur = (uint32_t)(*spu_reg(x, 0x1C06 + 0x10 * n)) * 8; if (!v->rep_valid) v->rep = v->cur;      /* a repeat address written before key-on survives it: the movie sound of the game depends on it (finding 0019) */
    v->counter = 0; v->old = v->older = 0; memset(v->s, 0, sizeof v->s);
    x->spu.endx &= ~(1u << n);
    spu_decode_block(x, v);
    v->cur += 16;
}

static int spu_adsr_step(IoCtx *x, int n, Voice *v) {              /* advance the envelope by one 44.1 kHz cycle, returns the level */
    uint16_t a1 = *spu_reg(x, 0x1C08 + 0x10 * n), a2 = *spu_reg(x, 0x1C0A + 0x10 * n);
    int exp = 0, dec = 0, shift = 0, step = 0, target = -1;
    switch (v->phase) {
    case 0: exp = a1 >> 15; shift = (a1 >> 10) & 0x1F; step = 7 - ((a1 >> 8) & 3); break;
    case 1: exp = 1; dec = 1; shift = (a1 >> 4) & 0xF; step = -8; target = ((a1 & 0xF) + 1) * 0x800; break;
    case 2: exp = a2 >> 15; dec = (a2 >> 14) & 1; shift = (a2 >> 8) & 0x1F; step = (a2 >> 6) & 3; step = dec ? -(8 - step) : 7 - step; break;
    default: exp = (a2 >> 5) & 1; dec = 1; shift = a2 & 0x1F; step = -8; break;
    }
    if (v->adsr_cnt > 0) { v->adsr_cnt--; return v->level; }
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int st = step * (1 << (11 > shift ? 11 - shift : 0));
    if (exp && !dec && v->level > 0x6000) cycles *= 4;
    if (exp && dec) st = (st * v->level) >> 15;                      /* arithmetic shift (floor): a small negative step still moves the level, so the exponential decrease reaches 0 instead of sticking (finding 0045) */
    v->adsr_cnt = cycles - 1;
    v->level += st;
    if (v->level < 0) v->level = 0;
    if (v->level > 0x7FFF) v->level = 0x7FFF;
    if (v->phase == 0 && v->level >= 0x7FFF) v->phase = 1;
    else if (v->phase == 1 && v->level <= target) v->phase = 2;
    else if (v->phase == 3 && v->level == 0) v->on = 0;
    return v->level;
}

static int fixvol(uint16_t r) {                                       /* fixed mode: volume/2 in bits 0-14; sweep mode: applied as its end target */
    if (!(r & 0x8000)) return (int)(int16_t)(r << 1);
    int mag = (r & 0x2000) ? 0 : 0x7FFF;
    return (r & 0x1000) ? -mag : mag;
}

static void spu_sample(IoCtx *x, int *ol, int *or_) {
    Spu *p = &x->spu;
    uint16_t cnt = *spu_reg(x, 0x1DAA);
    int lsum = 0, rsum = 0, revl = 0, revr = 0;
    uint32_t eon = *spu_reg(x, 0x1D98) | ((uint32_t)*spu_reg(x, 0x1D9A) << 16);
    uint32_t pmon = *spu_reg(x, 0x1D90) | ((uint32_t)*spu_reg(x, 0x1D92) << 16);
    uint32_t non = *spu_reg(x, 0x1D94) | ((uint32_t)*spu_reg(x, 0x1D96) << 16);
    /* noise generator */
    {
        int nshift = (cnt >> 10) & 0xF, nstep = ((cnt >> 8) & 3) + 4;
        int t = (int)p->noise_timer - nstep;
        int parity = (((p->noise_level >> 15) ^ (p->noise_level >> 12) ^ (p->noise_level >> 11) ^ (p->noise_level >> 10) ^ 1) & 1);
        p->noise_timer = (uint32_t)t;
        if (t < 0) { p->noise_level = (int16_t)(p->noise_level * 2 + parity); t += 0x20000 >> nshift; }
        if (t < 0) t += 0x20000 >> nshift;
        p->noise_timer = (uint32_t)t;
    }
    for (int n = 0; n < 24; n++) {
        Voice *v = &p->v[n];
        if (!v->on) { v->out = 0; continue; }
        int env = spu_adsr_step(x, n, v);
        int pitch = *spu_reg(x, 0x1C04 + 0x10 * n), step = pitch;
        if (n > 0 && (pmon >> n & 1)) { int f = p->v[n - 1].out + 0x8000; int sp = (int16_t)pitch; step = ((sp * f) >> 15) & 0xFFFF; }
        if (step > 0x3FFF) step = 0x4000;
        v->counter += (uint32_t)step;
        while (v->counter >= 0x1C000) {                              /* 28 samples consumed: next block */
            v->counter -= 0x1C000;
            int fl = v->blk_flags;
            spu_irq_check(x, v->cur);                                 /* the pointer has stepped past the block that carries the loop-end flag when the jump back happens: the movie sound buffers end on this (finding 0019) */
            if (fl & 1) {                                           /* loop end */
                p->endx |= 1u << n;
                v->cur = v->rep;
                if (!(fl & 2)) { v->phase = 3; v->level = 0; }      /* End+Mute: release, envelope 0 */
            }
            spu_decode_block(x, v);
            v->cur = (v->cur + 16) & 0x7FFFF;
        }
        int idx = (int)(v->counter >> 12), i8 = (int)((v->counter >> 4) & 0xFF);
        int smp;
        if ((non >> n) & 1) smp = (int16_t)p->noise_level;
        else {
            int s0 = v->s[idx + 0], s1 = v->s[idx + 1], s2 = v->s[idx + 2], s3 = v->s[idx + 3];      /* oldest .. new */
            smp = ((spu_gauss[0xFF - i8] * s0) >> 15) + ((spu_gauss[0x1FF - i8] * s1) >> 15) + ((spu_gauss[0x100 + i8] * s2) >> 15) + ((spu_gauss[i8] * s3) >> 15);
            smp = s16c(smp);
        }
        smp = (smp * env) >> 15;
        v->out = smp;
        int l = (smp * fixvol(*spu_reg(x, 0x1C00 + 0x10 * n))) >> 15, r = (smp * fixvol(*spu_reg(x, 0x1C02 + 0x10 * n))) >> 15;
        lsum += l; rsum += r;
        if ((eon >> n) & 1) { revl += l; revr += r; }
    }
    /* CD audio (XA-ADPCM) input at 44.1 kHz from the decoded ring */
    int cdl = 0, cdr = 0;
    if (p->xa_w != p->xa_r) {
        cdl = p->xa[0][p->xa_r & (XA_RING - 1)]; cdr = p->xa[1][p->xa_r & (XA_RING - 1)];
        p->xa_frac += 6;                                              /* 37800 / 44100 = 6/7 */
        if (p->xa_frac >= 7) { p->xa_frac -= 7; p->xa_r++; }
        if (p->xa_rate_div) { /* 18900 Hz sectors were expanded at decode time */ }
    }
    static int noxa = -1; if (noxa < 0) noxa = getenv("PS1_NOXA") != NULL;       /* debug aid: mute the CD/XA input to see how much of the output it makes */
    if (noxa) cdl = cdr = 0;
    if (cnt & 1) {
        lsum += (cdl * (int16_t)*spu_reg(x, 0x1DB0)) >> 15; rsum += (cdr * (int16_t)*spu_reg(x, 0x1DB2)) >> 15;
        if (cnt & 4) { revl += (cdl * (int16_t)*spu_reg(x, 0x1DB0)) >> 15; revr += (cdr * (int16_t)*spu_reg(x, 0x1DB2)) >> 15; }
    }
    /* reverb (psx-spx "SPU Reverb Formula"): left and right are computed on alternate 44.1 kHz cycles; all addresses are relative to the current buffer
     * address and wrapped into mBASE..7FFFEh; products are divided by 8000h */
    {
        uint32_t base = (uint32_t)*spu_reg(x, 0x1DA2) * 8;
        if (base < 0x1000 || base > 0x7FFFE) base = 0x7FFFE;
        uint32_t span = 0x80000 - base;
        int side = p->rev_phase;
        if (p->rev_addr < base) p->rev_addr = base;
        #define RV(r) ((int)(int16_t)*spu_reg(x, 0x1DC0 + 2 * (r)))
        #define RA(r) ((int32_t)((uint32_t)*spu_reg(x, 0x1DC0 + 2 * (r)) * 8))
        #define REVA(off) (base + (uint32_t)((((int64_t)(p->rev_addr - base) + (int64_t)(off)) % (int64_t)span + (int64_t)span) % (int64_t)span))
        #define REVR(off) ((int)(int16_t)(x->spuram[REVA(off) & 0x7FFFE] | (x->spuram[(REVA(off) & 0x7FFFE) + 1] << 8)))
        #define REVW(off, val) do { if ((cnt >> 7) & 1) { uint32_t aa = REVA(off) & 0x7FFFE; int vv = s16c(val); x->spuram[aa] = (uint8_t)vv; x->spuram[aa + 1] = (uint8_t)(vv >> 8); } } while (0)
        int in = ((side ? revr : revl) * RV(side ? 0x1F : 0x1E)) >> 15;
        int iir = RV(0x02), wall = RV(0x07);
        int32_t msame = RA(0x0A + side), dsame = RA(0x10 + side), mdiff = RA(0x12 + side), ddiff = RA(side ? 0x18 : 0x19);
        int old = REVR(msame - 2);
        int val = (((in + ((REVR(dsame) * wall) >> 15) - old) * iir) >> 15) + old;
        REVW(msame, val);
        old = REVR(mdiff - 2);
        val = (((in + ((REVR(ddiff) * wall) >> 15) - old) * iir) >> 15) + old;
        REVW(mdiff, val);
        int out = (RV(3) * REVR(RA(side ? 0x0D : 0x0C)) + RV(4) * REVR(RA(side ? 0x0F : 0x0E)) + RV(5) * REVR(RA(side ? 0x15 : 0x14)) + RV(6) * REVR(RA(side ? 0x17 : 0x16))) >> 15;
        int32_t ap1 = RA(side ? 0x1B : 0x1A), ap2 = RA(side ? 0x1D : 0x1C);
        int va1 = RV(8), va2 = RV(9), m = REVR(ap1 - RA(0));
        out = out - ((va1 * m) >> 15);
        REVW(ap1, out);
        out = ((s16c(out) * va1) >> 15) + m;
        m = REVR(ap2 - RA(1));
        out = out - ((va2 * m) >> 15);
        REVW(ap2, out);
        out = ((s16c(out) * va2) >> 15) + m;
        out = (s16c(out) * (int)(int16_t)*spu_reg(x, side ? 0x1D86 : 0x1D84)) >> 15;
        if (side) { p->rev_r = out; uint32_t na = (p->rev_addr + 2) & 0x7FFFE; p->rev_addr = na < base ? base : na; } else p->rev_l = out;
        p->rev_phase ^= 1;
    }
    static int norev = -1; if (norev < 0) norev = getenv("PS1_NOREV") != NULL;       /* debug aid: leave the reverb output out of the mix */
    if (!norev) { lsum += p->rev_l; rsum += p->rev_r; }
    int ml = (int16_t)(*spu_reg(x, 0x1D80) << 1), mr = (int16_t)(*spu_reg(x, 0x1D82) << 1);
    int l = (lsum * ml) >> 15, r = (rsum * mr) >> 15;
    if (!(cnt & 0x8000) || !(cnt & 0x4000)) { l = r = 0; }               /* soft reset / mute */
    *ol = s16c(l); *or_ = s16c(r);
}

static void spu_frame(IoCtx *x) {                                      /* one virtual frame = 735 samples */
    Spu *p = &x->spu;
    if (getenv("PS1_SPUREG") && (x->frames % 60) == 0) fprintf(stderr, "[spureg] frame %u cnt %04x main %04x/%04x cdvol %04x/%04x\n", (unsigned)x->frames, *spu_reg(x, 0x1DAA), *spu_reg(x, 0x1D80), *spu_reg(x, 0x1D82), *spu_reg(x, 0x1DB0), *spu_reg(x, 0x1DB2));
    for (int i = 0; i < 735; i++) {
        int l, r;
        spu_sample(x, &l, &r);
        uint32_t k = p->rhead & (SPU_RING - 1);
        p->ring[2 * k] = (int16_t)l; p->ring[2 * k + 1] = (int16_t)r;
        p->rhead++;
        if (p->rhead - p->rtail > SPU_RING) { p->rtail = p->rhead - SPU_RING; p->dropped++; }
        p->hash = (p->hash ^ (uint16_t)l) * 0x100000001b3ull; p->hash = (p->hash ^ (uint16_t)r) * 0x100000001b3ull;
        p->total++;
        if (p->cap) { int16_t fr[2] = {(int16_t)l, (int16_t)r}; fwrite(fr, 2, 2, p->cap); }
    }
}

/* XA-ADPCM sector -> stereo/mono samples queued for the mixer (psx-spx decode_sector); 18900 Hz data is doubled */
static void spu_xa_sector(IoCtx *x, const uint8_t *raw) {
    Spu *p = &x->spu;
    uint8_t coding = raw[19];
    int stereo = coding & 1, half = (coding >> 2) & 1, bits8 = (coding >> 4) & 1;
    if (bits8) return;                                                  /* 8 bit XA is not used by this game (unverified) */
    const uint8_t *src = raw + 24;
    for (int i = 0; i < 0x12; i++, src += 128) {
        for (int blk = 0; blk < 4; blk++) {
            int16_t out[2][28];
            for (int nib = 0; nib < 2; nib++) {
                uint8_t hd = src[4 + blk * 2 + nib];
                int shift = 12 - (hd & 0xF), filter = (hd & 0x30) >> 4;
                if (shift < 0) shift = 3;
                int ch = stereo ? nib : 0;
                for (int j = 0; j < 28; j++) {
                    int t = (src[16 + blk + j * 4] >> (nib * 4)) & 0xF;
                    t = t >= 8 ? t - 16 : t;
                    int smp = s16c(t * (1 << shift) + ((p->xa_old[ch] * adpcm_f0[filter] + p->xa_older[ch] * adpcm_f1[filter] + 32) / 64));
                    out[nib][j] = (int16_t)smp; p->xa_older[ch] = p->xa_old[ch]; p->xa_old[ch] = smp;
                }
            }
            for (int j = 0; j < (stereo ? 28 : 56); j++) {
                int16_t l = stereo ? out[0][j] : out[j / 28][j % 28], r = stereo ? out[1][j] : l;
                for (int k = 0; k < (half ? 2 : 1); k++) { p->xa[0][p->xa_w & (XA_RING - 1)] = l; p->xa[1][p->xa_w & (XA_RING - 1)] = r; p->xa_w++; if (p->xa_w - p->xa_r > XA_RING) p->xa_r = p->xa_w - XA_RING; }
            }
        }
    }
}

void io_spu_capture(IoCtx *x, const char *path) { x->spu.cap = fopen(path, "wb"); }
void io_spu_capture_close(IoCtx *x) { if (x->spu.cap) fclose(x->spu.cap); x->spu.cap = NULL; }
int io_spu_pull(IoCtx *x, int16_t *out, int max_frames) {
    Spu *p = &x->spu;
    int n = 0;
    while (n < max_frames && p->rtail != p->rhead) { uint32_t k = p->rtail & (SPU_RING - 1); out[2 * n] = p->ring[2 * k]; out[2 * n + 1] = p->ring[2 * k + 1]; p->rtail++; n++; }
    return n;
}
uint64_t io_spu_total(IoCtx *x) { return x->spu.total; }
uint64_t io_spu_hash(IoCtx *x) { return x->spu.hash; }
