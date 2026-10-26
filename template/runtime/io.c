/* Register-level model of the parts of the PS1 hardware the game libraries touch (queue P3-2/P4-2). Source: psx-spx (nocash), sections
 * "Interrupt Control", "DMA Channels", "GPU Rendering Commands", "GPU Display Control Commands", "GPUSTAT". Timing is not modelled: a DMA
 * transfer completes inside the register write that starts it (decisions/0002 section 3.5: DrawSync completes immediately). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io.h"
#include "rt.h"

#define VW 1024
#define VH 512

typedef struct {
    uint32_t madr, bcr, chcr;
} Chan;

/* ---- CD-ROM controller (queue P4-4): psx-spx "CDROM Controller I/O Ports / Command Summary / Control, Seek, Read commands". Time = virtual
 * frames (io_vblank_tick); single data track image, XA audio sectors are consumed and dropped until the SPU model (P4-3) takes them. ---- */
#define CDQ 16
typedef struct { unsigned due; int type; int n; uint8_t b[16]; int sector; } CdEv;
typedef struct {
    FILE *f; char path[1024];
    int index, irq_en, irq_fl, mode, motor, reading, filt_file, filt_ch, adpmute;
    uint8_t param[16]; int nparam;
    uint8_t resp[16]; int nresp, presp;
    uint8_t sec[2352]; int sec_len, sec_off, sec_valid, data_ready, data_pos;
    uint8_t hdr[8];                                     /* header + subheader of the last sector read (GetlocL) */
    int target, cur;                                    /* setloc target, current sector */
    unsigned tick, next_sector, credit;
    CdEv q[CDQ]; int nq;
    uint8_t vol[4];
} Cd;

/* ---- MDEC (queue P4-5): psx-spx "MDEC I/O Ports / Commands / Decompression". Decoding happens when the last parameter word arrives; the output
 * is kept as a byte stream (macroblocks in raster order, 16x16 pixels each for colour modes) that DMA1 or the data register reads. ---- */
typedef struct {
    uint32_t status_cmd;                     /* bits 23-26 and 0-15 of the last command (status mirror) */
    int en_in, en_out;
    int state;                               /* 0 idle, 1 receiving decode data, 2 receiving quant tables, 3 receiving scale table */
    uint32_t cmd; int remain;                /* parameter words still expected */
    uint16_t *in; int nin, cap;              /* halfwords of the decode command */
    uint8_t iq_y[64], iq_uv[64]; int16_t scale[64];
    uint8_t qbuf[128]; int nq, qlen;
    uint8_t *out; int nout, outcap, outpos;  /* decoded bytes and read position */
} Mdec;

#include "spu_types.inc"

typedef struct { int x, y; int r, g, b; int u, v; } Vtx;
struct IoCtx {
    Spu spu;
    Cd cd;
    Mdec md;
    struct { uint16_t mode, ctrl, baud; uint8_t rx[8]; int nrx, state, irq_pend, busy, p_ack; uint8_t p_rx; uint16_t pad; uint32_t irq_flag; } sio;      /* SIO0: controller port 1 (finding 0006) */
    uint8_t *ram;
    char err[160];
    uint32_t i_stat, i_mask; int spu_irqflag;
    Chan ch[7];
    uint32_t d0_madr, d0_left; int d0_pend, d0_hold;  /* d0_hold: the input transfer has been consumed but stays busy until the decoded output has been read; MDEC input DMA waiting for the decoder to drain its output (finding 0010) */
    uint32_t d1_madr, d1_left; int d1_pend;           /* MDEC output DMA waiting for decoded data (finding 0010) */
    uint32_t dpcr, dicr, dma_pend;                      /* dma_pend: channels whose completion interrupt is queued (raised by io_dma_irq_next) */
    /* GPU */
    uint16_t *vram;
    uint32_t stat_texpage;      /* GPUSTAT bits 0-10 and 15 (from GP0 E1h) */
    uint32_t mask_bits;         /* GPUSTAT 11-12 (E6h) */
    uint32_t disp_bits;         /* GPUSTAT 14, 16-22 (GP1(08h)) */
    uint32_t disp_off, dma_dir, irq1;
    uint32_t disp_start, hrange, vrange;
    uint32_t draw_tl, draw_br, draw_ofs, tw;
    uint32_t e1_flip;           /* E1 bits 12/13: textured-rectangle X/Y flip */
    uint16_t clut_c[256]; int clut_n, clut_x, clut_y;   /* GPU CLUT cache: palette entries loaded for 4/8-bit textures (finding 0043) */
    uint32_t tex_dis_ok;        /* GP1(09h) bit 0: E1/texpage bit 11 may set GPUSTAT bit 15 (texture disable) */
    uint32_t cmd[16 + 1];       /* GP0 command being collected */
    unsigned ncmd, need;
    int polyline;
    int pl_active, pl_state, pl_op, pl_nv; Vtx pl_prev; int pl_r, pl_g, pl_b;      /* poly-line in progress (finding 0032) */
    int xfer;                   /* 0 none, 1 CPU->VRAM, 2 VRAM->CPU */
    int tx, ty, tw_, th, cx, cy, tpos;
    uint32_t gpuread;
    uint64_t c_words, c_prims, c_uploads, c_fills, c_dma2, c_ot;
    uint32_t t_pend[3], t_done[3];                  /* timer interrupts raised during the frame and not yet delivered; one-shot latch */
    uint32_t t_sub[3];             /* time that passes inside a virtual frame, driven by reads of the counter value (finding 0007) */
    uint32_t t_value[3], t_mode[3], t_target[3];   /* root counters 0..2: storage; timer 1 (hblank) and 2 (sysclk/8) advance in io_vblank_tick, timer 0 does not move (unproven) */
    uint64_t frames;
    uint32_t memctl[9];        /* 0x1f801000..0x1f801020 "Memory Control 1": plain storage, boot values from psx-spx ("usually") */
    uint16_t spu_r[0x100];
    uint8_t *spuram;           /* 512 KiB sound RAM */
    uint32_t spu_addr;         /* current transfer byte address (register 0x1f801da6 * 8) */       /* SPU registers 0x1f801c00..0x1f801dff as plain storage (queue P4-3 models the audio); SPUSTAT mirrors SPUCNT */
    uint64_t c_spu_w;
};

static int bad(IoCtx *x, const char *fmt, uint32_t a, unsigned size) {
    snprintf(x->err, sizeof x->err, fmt, a, size);
    return 0;
}
const char *io_last_error(IoCtx *x) { return x->err; }

IoCtx *io_new(uint8_t *ram) {
    IoCtx *x = calloc(1, sizeof *x);
    x->ram = ram;
    x->vram = calloc((size_t)VW * VH, 2);
    x->spuram = calloc(1, 0x80000);
    x->dpcr = 0x07654321u;
    x->cd.irq_en = 0x1F;          /* set by the BIOS at boot (psx-spx: "usually all set") */
    { static const uint32_t mc[9] = {0x1F000000, 0x1F802000, 0x0013243F, 0x00003022, 0x0013243F, 0x200931E1, 0x00020843, 0x00070777, 0x00031125}; memcpy(x->memctl, mc, sizeof mc); }
    x->sio.pad = 0xFFFF;        /* no buttons pressed (active low) */
    x->disp_off = 1;            /* display disabled after reset (GP1(00h)) */
    x->stat_texpage = 0;
    return x;
}
void io_free(IoCtx *x) { if (x) { free(x->vram); free(x->spuram); free(x); } }
uint16_t *io_vram(IoCtx *x) { return x->vram; }

static uint16_t *spu_reg(IoCtx *x, uint32_t off) { return &x->spu_r[(off - 0x1C00) >> 1]; }
#include "spu.inc"

/* ---------------------------------------------------------------- CD-ROM ---- */
static uint8_t cd_stat(IoCtx *x) { return (uint8_t)((x->cd.motor ? 0x02 : 0) | (x->cd.reading ? 0x20 : 0)); }
static void cd_irq_raise(IoCtx *x) { if (x->cd.irq_fl & x->cd.irq_en) { x->i_stat |= 1u << 2; COV(RT_COV_IRQ, 2); } }
static void cd_queue(IoCtx *x, unsigned delay, int type, const uint8_t *b, int n, int sector) {
    Cd *c = &x->cd;
    if (c->nq == CDQ) return;
    CdEv *e = &c->q[c->nq++];
    e->due = c->tick + delay; e->type = type; e->n = n; e->sector = sector;
    for (int i = 0; i < n; i++) e->b[i] = b[i];
}
static int bcd2(int v) { return ((v / 10) << 4) | (v % 10); }
static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

static void cd_read_sector(IoCtx *x) {                  /* load the sector at cur into the host buffer; returns with sec_valid = 0 on failure */
    Cd *c = &x->cd;
    c->sec_valid = 0;
    if (!c->f || c->cur < 0) return;
    if (fseek(c->f, (long)c->cur * 2352, SEEK_SET) != 0 || fread(c->sec, 1, 2352, c->f) != 2352) return;
    memcpy(c->hdr, c->sec + 12, 8);                      /* header (mm ss ff mode) + first 4 subheader bytes */
    c->sec_valid = 1;
}

#ifdef RT_CALLTRACE
extern uint32_t rt_ct[4096]; extern unsigned rt_cti;
#endif
static void cd_command(IoCtx *x, int cmd) {
    Cd *c = &x->cd;
#ifdef RT_CALLTRACE
    if (cmd == 2 && getenv("PS1_CDRING") && c->tick >= (unsigned)atoi(getenv("PS1_CDTICK") ? getenv("PS1_CDTICK") : "0")) {   /* debug aid (build with --calltrace): the last N function entries at every Setloc, consecutive repeats collapsed */
        unsigned n = (unsigned)atoi(getenv("PS1_CDRING")); if (n > 4096) n = 4096;
        fprintf(stderr, "[cdring] tick %u setloc:", c->tick);
        for (unsigned i = 0, last = 0; i < n; i++) { unsigned v = rt_ct[(rt_cti + 4096 - n + i) & 4095]; if (v != last) fprintf(stderr, " %08x", v); last = v; }
        fprintf(stderr, "\n");
    }
#endif
    COV(RT_COV_CD_CMD, cmd);
    if (getenv("PS1_CDLOG")) { fprintf(stderr, "[cd] tick %u cmd %02x params", c->tick, cmd); for (int i = 0; i < c->nparam; i++) fprintf(stderr, " %02x", c->param[i]); fprintf(stderr, "  (target %d cur %d mode %02x)\n", c->target, c->cur, c->mode); }
    uint8_t st = cd_stat(x), r[16];
    int np = c->nparam;
    switch (cmd) {
    case 0x01: r[0] = st; cd_queue(x, 0, 3, r, 1, 0); break;
    case 0x02: {
        int m = unbcd(c->param[0]), s = unbcd(c->param[1]), f = unbcd(c->param[2]);
        c->target = (m * 60 + s) * 75 + f - 150;
        r[0] = st; cd_queue(x, 0, 3, r, 1, 0); break;
    }
    case 0x06: case 0x1B:                                /* ReadN / ReadS */
        r[0] = st; cd_queue(x, 0, 3, r, 1, 0);
        c->motor = 1; c->reading = 1; c->cur = c->target; c->credit = 0;
        c->next_sector = c->tick + 2;                    /* seek time */
        break;
    case 0x09:                                           /* Pause */
        r[0] = st; cd_queue(x, 0, 3, r, 1, 0);
        c->reading = 0;
        r[0] = cd_stat(x); cd_queue(x, 2, 2, r, 1, 0); break;
    case 0x08:                                           /* Stop */
        r[0] = st; cd_queue(x, 0, 3, r, 1, 0);
        c->reading = 0; c->motor = 0;
        r[0] = cd_stat(x); cd_queue(x, 2, 2, r, 1, 0); break;
    case 0x07: r[0] = st; cd_queue(x, 0, 3, r, 1, 0); c->motor = 1; r[0] = cd_stat(x); cd_queue(x, 2, 2, r, 1, 0); break;
    case 0x0A:                                           /* Init */
        c->mode = 0x20; c->motor = 1; c->reading = 0; c->nq = 0;
        r[0] = cd_stat(x); cd_queue(x, 0, 3, r, 1, 0); cd_queue(x, 2, 2, r, 1, 0); break;
    case 0x0B: case 0x0C: c->adpmute = cmd == 0x0B; r[0] = st; cd_queue(x, 0, 3, r, 1, 0); break;
    case 0x0D: c->filt_file = c->param[0]; c->filt_ch = c->param[1]; r[0] = st; cd_queue(x, 0, 3, r, 1, 0); break;
    case 0x0E: c->mode = c->param[0]; r[0] = st; cd_queue(x, 0, 3, r, 1, 0); break;
    case 0x0F: r[0] = st; r[1] = (uint8_t)c->mode; r[2] = 0; r[3] = (uint8_t)c->filt_file; r[4] = (uint8_t)c->filt_ch; cd_queue(x, 0, 3, r, 5, 0); break;
    case 0x10:                                           /* GetlocL: header of the last sector read */
        for (int i = 0; i < 8; i++) r[i] = c->hdr[i];
        r[3] = c->hdr[3]; cd_queue(x, 0, 3, r, 8, 0); break;
    case 0x11: {                                         /* GetlocP (approximation: track 1, absolute position of the current sector) */
        int a = c->cur + 150, rel = c->cur;
        r[0] = 1; r[1] = 1; r[2] = (uint8_t)bcd2(rel / 4500); r[3] = (uint8_t)bcd2((rel / 75) % 60); r[4] = (uint8_t)bcd2(rel % 75);
        r[5] = (uint8_t)bcd2(a / 4500); r[6] = (uint8_t)bcd2((a / 75) % 60); r[7] = (uint8_t)bcd2(a % 75);
        cd_queue(x, 0, 3, r, 8, 0); break;
    }
    case 0x13: r[0] = st; r[1] = 0x01; r[2] = 0x01; cd_queue(x, 0, 3, r, 3, 0); break;          /* GetTN: one track */
    case 0x14: {                                         /* GetTD(track): track 1 starts at 00:02, track 0 / end = disc length */
        int t = unbcd(c->param[0]);
        r[0] = st;
        if (t == 1) { r[1] = 0x00; r[2] = 0x02; }
        else { int end = 262441 + 150; r[1] = (uint8_t)bcd2(end / 4500); r[2] = (uint8_t)bcd2((end / 75) % 60); }
        cd_queue(x, 0, 3, r, 3, 0); break;
    }
    case 0x15: case 0x16:                                /* SeekL / SeekP */
        r[0] = st; cd_queue(x, 0, 3, r, 1, 0); c->motor = 1; c->cur = c->target; c->reading = 0;
        r[0] = cd_stat(x); cd_queue(x, 3, 2, r, 1, 0); break;
    case 0x19:                                           /* Test: only sub 20h (version) */
        if (c->param[0] == 0x20) { r[0] = 0x94; r[1] = 0x09; r[2] = 0x19; r[3] = 0xC0; cd_queue(x, 0, 3, r, 4, 0); }
        else { r[0] = (uint8_t)(st | 1); r[1] = 0x10; cd_queue(x, 0, 5, r, 2, 0); }
        break;
    case 0x1A:                                           /* GetID: licensed data disc, region SCEA */
        r[0] = st; cd_queue(x, 0, 3, r, 1, 0);
        r[0] = 0x02; r[1] = 0x00; r[2] = 0x20; r[3] = 0x00; r[4] = 'S'; r[5] = 'C'; r[6] = 'E'; r[7] = 'A';
        cd_queue(x, 3, 2, r, 8, 0); break;
    case 0x1E: r[0] = st; cd_queue(x, 0, 3, r, 1, 0); r[0] = st; cd_queue(x, 4, 2, r, 1, 0); break;   /* ReadTOC */
    default:                                             /* invalid command: INT5(stat|1, 40h), parameters are NOT consumed */
        r[0] = (uint8_t)(st | 1); r[1] = 0x40; cd_queue(x, 0, 5, r, 2, 0); np = -1; break;
    }
    if (np >= 0) c->nparam = 0;
}

/* fire the first due event if no interrupt is pending; returns 1 if one fired */
static int cd_fire(IoCtx *x) {
    Cd *c = &x->cd;
    if (c->irq_fl) return 0;
    /* sector delivery while reading (1.25 sectors per frame, 2.5 in double speed mode) */
    if (c->reading && c->nq == 0 && c->tick >= c->next_sector) {
        if (c->credit >= 4) {
            c->credit -= 4;
            cd_read_sector(x);
            uint8_t r[1] = {cd_stat(x)};
            int deliver = c->sec_valid;
            if (deliver) {
                uint8_t sm = c->hdr[6];                       /* subheader submode */
                COV(RT_COV_CD_SECTOR, (sm >> 1) & 7);       /* submode bits 1..3: video / audio / data */
                int xa = (c->mode & 0x40) && c->hdr[3] == 2 && (sm & 0x44) == 0x44;      /* audio + real-time sector for the ADPCM decoder */
                if (xa && (!(c->mode & 0x08) || (c->hdr[4] == c->filt_file && c->hdr[5] == c->filt_ch))) { spu_xa_sector(x, c->sec); deliver = 0; }   /* consumed by the XA decoder */
                else if ((c->mode & 0x08) && (sm & 0x44) == 0x44) deliver = 0;               /* filtered-out audio sector is not delivered as data either */
            }
            if (getenv("PS1_CDLOG") && getenv("PS1_CDLOG")[0] == '2') fprintf(stderr, "[cd] tick %u sector %d hdr %02x %02x %02x %02x sub %02x %02x %02x %02x deliver %d\n", c->tick, c->cur, c->hdr[0], c->hdr[1], c->hdr[2], c->hdr[3], c->hdr[4], c->hdr[5], c->hdr[6], c->hdr[7], deliver);
            c->cur++;
            if (deliver) {
                c->sec_len = (c->mode & 0x20) ? 0x924 : 0x800;
                c->sec_off = (c->mode & 0x20) ? 12 : 24;
                cd_queue(x, 0, 1, r, 1, 1);
            }
        }
    }
    if (c->nq == 0 || c->q[0].due > c->tick) return 0;
    CdEv e = c->q[0];
    memmove(&c->q[0], &c->q[1], (size_t)(c->nq - 1) * sizeof c->q[0]);
    c->nq--;
    c->nresp = e.n; c->presp = 0;
    memset(c->resp, 0, sizeof c->resp);
    memcpy(c->resp, e.b, (size_t)e.n);
    c->irq_fl = e.type;
    if (e.sector) { c->data_ready = 0; c->data_pos = 0; }
    cd_irq_raise(x);
    return 1;
}

int io_cd_open(IoCtx *x, const char *path) { snprintf(x->cd.path, sizeof x->cd.path, "%s", path); x->cd.f = fopen(path, "rb"); return x->cd.f != NULL; }
int io_cd_reopen(IoCtx *x) { if (x->cd.f) fclose(x->cd.f); x->cd.f = fopen(x->cd.path, "rb"); return x->cd.f != NULL; }      /* a forked child must not share the open file description (and offset) of its siblings */
static void dicr_update(IoCtx *x, int ch);
int io_cd_step(IoCtx *x) { return cd_fire(x); }
/* DMA completion interrupts: a transfer is instantaneous in this model, but its interrupt must not be visible inside the interrupt handler that started it (a handler that restarts the
 * next transfer, like an MDEC or CD streaming callback, would otherwise see the flag again at once and loop for ever; real transfers take time). Queued per channel, raised one per call
 * between the program's interrupt handlers (finding 0003). Returns 1 if an interrupt reached I_STAT. */
int io_dma_pending(const IoCtx *x) { return x->dma_pend != 0; }
int io_dma_irq_next(IoCtx *x) {
    for (int ch = 0; ch < 7; ch++) if (x->dma_pend & (1u << ch)) {
        x->dma_pend &= ~(1u << ch);
        uint32_t before = x->i_stat;
        dicr_update(x, ch);
        if (x->i_stat != before) return 1;
    }
    return 0;
}

static uint32_t cd_read_reg(IoCtx *x, uint32_t p) {
    Cd *c = &x->cd;
    switch (p & 3) {
    case 0: {
        uint32_t v = (uint32_t)c->index;
        v |= (c->nparam == 0) << 3;
        v |= (c->nparam < 16) << 4;
        v |= (c->presp < c->nresp) << 5;
        v |= (c->data_ready && c->data_pos < c->sec_len) << 6;
        return v;
    }
    case 1: { uint32_t v = c->presp < c->nresp ? c->resp[c->presp] : 0; if (c->presp < c->nresp) c->presp++; return v; }
    case 2: {
        uint32_t v = 0;
        if (c->data_ready && c->sec_valid) {
            int i = c->data_pos < c->sec_len ? c->data_pos : c->sec_len - 8;
            v = c->sec[c->sec_off + i];
            if (c->data_pos < c->sec_len) c->data_pos++;
        }
        return v;
    }
    default: return (c->index & 1) ? (uint32_t)(c->irq_fl | 0xE0) : (uint32_t)(c->irq_en | 0xE0);
    }
}

static void cd_write_reg(IoCtx *x, uint32_t p, uint32_t v) {
    Cd *c = &x->cd;
    v &= 0xFF;
    if (getenv("PS1_CDVOL") && ((p & 3) >= 1) && (c->index >= 2)) fprintf(stderr, "[cdvol] tick %u idx %d reg %d = %02x\n", c->tick, c->index, (int)(p & 3), v);
    switch (p & 3) {
    case 0: c->index = (int)(v & 3); break;
    case 1:
        if (c->index == 0) cd_command(x, (int)v);
        else if (c->index == 3) c->vol[3] = (uint8_t)v;
        break;
    case 2:
        if (c->index == 0) { if (c->nparam < 16) c->param[c->nparam++] = (uint8_t)v; }
        else if (c->index == 1) { c->irq_en = (int)(v & 0x1F); if (c->irq_fl & c->irq_en) x->i_stat |= 1u << 2; }
        else if (c->index == 2) c->vol[0] = (uint8_t)v;
        else c->vol[2] = (uint8_t)v;
        break;
    case 3:
        if (c->index == 0) {                                    /* request register: BFRD loads / resets the data FIFO */
            if (v & 0x80) { if (c->sec_valid && (!c->data_ready || c->data_pos >= c->sec_len)) { c->data_ready = 1; c->data_pos = 0; } }      /* BFRD loads the FIFO only when it is empty: a partly read sector keeps its read position (a reader that takes the 12 header bytes and then the 2048 data bytes in two DMAs depends on it; DuckStation cdrom.cpp, finding 0002) */
            else { c->data_ready = 0; }
        } else if (c->index == 1) {                             /* interrupt flag acknowledge, parameter FIFO reset */
            c->irq_fl &= ~(int)(v & 0x1F);
            if (v & 0x40) c->nparam = 0;
            if (v & 0x1F) { c->nresp = 0; c->presp = 0; }
        } else if (c->index == 2) c->vol[1] = (uint8_t)v;
        break;
    }
}

/* ---------------------------------------------------------------- MDEC ---- */
static const uint8_t zigzag[64] = {0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
                                   10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

static int sgn10(int v) { v &= 0x3FF; return v >= 0x200 ? v - 0x400 : v; }

/* decode one 8x8 block from the halfword stream; returns the new read index or -1 when the stream ends early */
static void mdec_dma1_resume(IoCtx *x);
static void mdec_dma0_resume(IoCtx *x);
static int mdec_block(Mdec *m, int *src, int blk[64], const uint8_t *qt) {
    memset(blk, 0, 64 * sizeof(int));
    int n;
    do { if (*src >= m->nin) return -1; n = m->in[(*src)++]; } while (n == 0xFE00);
    int qscale = (n >> 10) & 0x3F, k = 0;
    int val = sgn10(n) * qt[0];
    for (;;) {
        if (qscale == 0) val = sgn10(n) * 2;
        if (val < -0x400) val = -0x400; else if (val > 0x3FF) val = 0x3FF;
        if (qscale > 0) { /* zagzig: block position of coefficient k is the index i with zigzag[i] == k */
            for (int i = 0; i < 64; i++) if (zigzag[i] == k) { blk[i] = val; break; }
        } else blk[k] = val;
        if (k == 63) break;                                   /* all 64 coefficients present: the block is complete without an end-of-block word (the public ps1-tests heart stream has none, finding 0043) */
        if (*src >= m->nin) return -1;
        n = m->in[(*src)++];
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (sgn10(n) * qt[k] * qscale + 4) / 8;
    }
    /* IDCT with the scale table, two passes in full precision; the table is used as uploaded (row = frequency, psx-spx), the second pass rounds the 64-bit sum
     * at bit 32 and the result is limited to a signed 9 bit value and then to -128..127. psx-spx only gives an approximation ("(sum+0fffh)/2000h ... results aren't
     * perfect"); this is the variant that agrees with a hardware-derived reference (finding 0028). */
    int64_t tmp[64];
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        int64_t sum = 0;
        for (int u = 0; u < 8; u++) sum += (int64_t)blk[u * 8 + x] * (int16_t)m->scale[u * 8 + y];
        tmp[x + y * 8] = sum;
    }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        int64_t sum = 0;
        for (int u = 0; u < 8; u++) sum += tmp[u + y * 8] * (int16_t)m->scale[u * 8 + x];
        int v = (int)((sum >> 32) + ((sum >> 31) & 1));
        v = ((v & 0x1FF) ^ 0x100) - 0x100;                 /* sign-extend from 9 bits */
        blk[x + y * 8] = v < -128 ? -128 : v > 127 ? 127 : v;
    }
    return 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void mdec_decode(Mdec *m) {
    int depth = (m->cmd >> 27) & 3, sgn = (m->cmd >> 26) & 1, b15 = (m->cmd >> 25) & 1;
    int src = 0, cr[64], cb[64], yb[4][64];
    m->nout = 0; m->outpos = 0;
    for (;;) {
        size_t need = depth >= 2 ? 768 : 64;
        if ((size_t)m->nout + need > (size_t)m->outcap) { m->outcap = m->outcap ? m->outcap * 2 : 1 << 20; if ((size_t)m->nout + need > (size_t)m->outcap) m->outcap = (int)(m->nout + need); m->out = realloc(m->out, (size_t)m->outcap); }
        uint8_t *o = m->out + m->nout;
        if (depth < 2) {                                   /* monochrome: one Y block per macroblock */
            if (mdec_block(m, &src, yb[0], m->iq_y) < 0) break;
            for (int i = 0; i < 64; i++) { int y = clampi((int)((uint32_t)yb[0][i] << 23) >> 23, -128, 127); if (!sgn) y ^= 0x80; o[i] = (uint8_t)y; }
            m->nout += 64;                                 /* 8 bit; 4 bit mode packs nibbles below */
            if (depth == 0) { for (int i = 0; i < 32; i++) o[i] = (uint8_t)(((o[2 * i] >> 4) & 15) | (o[2 * i + 1] & 0xF0)); m->nout -= 32; }
            continue;
        }
        if (mdec_block(m, &src, cr, m->iq_uv) < 0) break;
        if (mdec_block(m, &src, cb, m->iq_uv) < 0) break;
        for (int k = 0; k < 4; k++) if (mdec_block(m, &src, yb[k], m->iq_y) < 0) goto done;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            int bi = (y / 8) * 2 + (x / 8);
            int Y = yb[bi][(x % 8) + (y % 8) * 8], r = cr[(x / 2) + (y / 2) * 8], b = cb[(x / 2) + (y / 2) * 8];
            int R = clampi(Y + (int)(1.402f * (float)r), -128, 127);          /* single precision, truncated toward zero */
            int G = clampi(Y + (int)((-0.3437f * (float)b) + (-0.7143f * (float)r)), -128, 127);
            int B = clampi(Y + (int)(1.772f * (float)b), -128, 127);
            if (!sgn) { R ^= 0x80; G ^= 0x80; B ^= 0x80; }
            R &= 0xFF; G &= 0xFF; B &= 0xFF;
            uint8_t *p = o + (size_t)(y * 16 + x) * (depth == 2 ? 3 : 2);
            if (depth == 2) { p[0] = (uint8_t)R; p[1] = (uint8_t)G; p[2] = (uint8_t)B; }
            else { uint16_t px = (uint16_t)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10) | (b15 << 15)); p[0] = (uint8_t)px; p[1] = (uint8_t)(px >> 8); }
        }
        m->nout += depth == 2 ? 768 : 512;
    }
done:;
}

static void mdec_write_word(IoCtx *x, uint32_t w) {
    Mdec *m = &x->md;
    if (m->state == 0) {
        unsigned c = w >> 29;
        COV(RT_COV_MDEC, c);
        m->cmd = w;
        m->status_cmd = (w & 0x1E000000u) | (w & 0xFFFF);
        if (getenv("PS1_MDECLOG")) fprintf(stderr, "[mdec] command word %08x (cmd %u)\n", w, c);
        if (c == 1) { m->remain = (int)(w & 0xFFFF); m->nin = 0; m->state = m->remain ? 1 : 0; if (!m->remain) m->nout = m->outpos = 0; }
        else if (c == 2) { m->qlen = (w & 1) ? 128 : 64; m->nq = 0; m->remain = m->qlen / 4; m->state = 2; }
        else if (c == 3) { m->qlen = 128; m->nq = 0; m->remain = 32; m->state = 3; }
        return;
    }
    if (m->state == 1) {
        if (m->nin + 2 > m->cap) { m->cap = m->cap ? m->cap * 2 : 1 << 16; m->in = realloc(m->in, (size_t)m->cap * 2); }
        m->in[m->nin++] = (uint16_t)w; m->in[m->nin++] = (uint16_t)(w >> 16);
        if (--m->remain == 0) { mdec_decode(m); m->state = 0; mdec_dma1_resume(x); if (getenv("PS1_MDECLOG")) fprintf(stderr, "[mdec] decoded: %d input halfwords -> %d output bytes\n", m->nin, m->nout); }
    } else {
        for (int b = 0; b < 4 && m->nq < 128; b++) m->qbuf[m->nq++] = (uint8_t)(w >> (8 * b));
        if (--m->remain == 0) {
            if (m->state == 2) { memcpy(m->iq_y, m->qbuf, 64); if (m->qlen == 128) memcpy(m->iq_uv, m->qbuf + 64, 64); }
            else for (int i = 0; i < 64; i++) m->scale[i] = (int16_t)(m->qbuf[2 * i] | (m->qbuf[2 * i + 1] << 8));
            m->state = 0;
        }
    }
}

static uint32_t mdec_read_word(IoCtx *x) {
    Mdec *m = &x->md;
    uint32_t w = 0;
    for (int b = 0; b < 4; b++) if (m->outpos < m->nout) w |= (uint32_t)m->out[m->outpos++] << (8 * b);
    return w;
}
static void mdec_dma0_release(IoCtx *x) {                   /* the input transfer of a frame ends when the whole decoded frame has been read out (the reference's behaviour; the program's callbacks depend on it, finding 0010) */
    if (!x->d0_hold || x->md.outpos < x->md.nout) return;
    x->d0_hold = 0;
    x->ch[0].chcr &= ~((1u << 24) | (1u << 28)); x->dma_pend |= 1u << 0;
}
static void mdec_dma0_resume(IoCtx *x) {
    mdec_dma0_release(x);
    if (!x->d0_pend || x->md.outpos < x->md.nout) return;
    x->d0_pend = 0;
    uint32_t a = x->d0_madr;
    for (uint32_t k = 0; k < x->d0_left; k++, a = (a + 4) & 0x1FFFFC) { uint32_t w; memcpy(&w, x->ram + a, 4); mdec_write_word(x, w); }
    if (x->md.outpos < x->md.nout) { x->d0_hold = 1; return; }
    x->ch[0].chcr &= ~((1u << 24) | (1u << 28)); x->dma_pend |= 1u << 0;
}
static void mdec_dma1_resume(IoCtx *x) {                    /* called when the decoder has produced more output: continue a waiting MDEC -> RAM transfer */
    if (!x->d1_pend) return;
    uint32_t avail = (uint32_t)((x->md.nout - x->md.outpos) / 4), take = x->d1_left < avail ? x->d1_left : avail;
    uint32_t a = x->d1_madr;
    for (uint32_t k = 0; k < take; k++, a = (a + 4) & 0x1FFFFC) { uint32_t w = mdec_read_word(x); memcpy(x->ram + a, &w, 4); }
    x->d1_madr = a; x->d1_left -= take;
    if (x->d1_left == 0) { x->d1_pend = 0; x->ch[1].chcr &= ~((1u << 24) | (1u << 28)); x->dma_pend |= 1u << 1; mdec_dma0_resume(x); }
}
static uint32_t mdec_status(IoCtx *x) {
    Mdec *m = &x->md;
    uint32_t s = m->status_cmd;
    s |= (uint32_t)(m->outpos >= m->nout) << 31;
    s |= (uint32_t)(m->state != 0 || m->outpos < m->nout) << 29;   /* busy: receiving, or executing = decoded data not yet read out (the reference keeps it set until the last block is consumed; the program polls it between strips, finding 0010) */
    s |= (uint32_t)m->en_in << 28;
    s |= (uint32_t)(m->en_out && m->outpos < m->nout) << 27;
    s = (s & ~0xFFFFu) | (uint32_t)((m->state ? m->remain - 1 : -1) & 0xFFFF);
    return s;
}

/* ---------------------------------------------------------------- GPU ---- */
static uint32_t gpustat(IoCtx *x) {
    uint32_t s = x->stat_texpage | x->mask_bits | x->disp_bits;
    if (!(x->disp_bits & (1u << 22))) s |= 1u << 13;         /* "always 1 when GP1(08h).5 = 0" */
    s |= (x->disp_off & 1u) << 23;
    if (x->disp_bits & (1u << 22)) s |= ((uint32_t)x->frames & 1u) << 31;   /* bit 31: odd/even field in vertical-interlace mode, one field per frame (a program may busy-wait on it); 0 otherwise */
    s |= (x->irq1 & 1u) << 24;
    s |= (1u << 26) | (1u << 28);                            /* ready for command, write FIFO empty: transfers are instantaneous */
    if (x->xfer == 2) s |= 1u << 27;
    s |= (x->dma_dir & 3u) << 29;
    uint32_t drq = 0;
    switch (x->dma_dir & 3u) { case 1: drq = 1; break; case 2: drq = (s >> 28) & 1; break; case 3: drq = (s >> 27) & 1; break; }
    s |= drq << 25;
    return s;
}

static void vram_put(IoCtx *x, int px, int py, uint16_t v) {           /* CPU->VRAM and VRAM->VRAM writes obey the E6 mask settings (public ps1-tests mask-bit, finding 0043); the fill below bypasses this */
    uint16_t *d = &x->vram[(py & (VH - 1)) * VW + (px & (VW - 1))];
    if ((x->mask_bits & (2u << 11)) && (*d & 0x8000)) return;           /* E6.1: do not overwrite pixels whose mask bit is set */
    if (x->mask_bits & (1u << 11)) v |= 0x8000;                         /* E6.0: set the mask bit of what is written */
    *d = v;
}
static void vram_fill_put(IoCtx *x, int px, int py, uint16_t v) { x->vram[(py & (VH - 1)) * VW + (px & (VW - 1))] = v; }

static void xfer_word(IoCtx *x, uint32_t w);

static unsigned cmd_len(IoCtx *x, uint32_t c0) {
    unsigned op = c0 >> 24;
    x->polyline = 0;
    /* GP0 NOPs: 00h, 04h..1Eh, E0h, E7h..EFh (psx-spx "GPU Other Commands": mirrors of GP0(00h)), 03h timing control, 01h cache clear, 1Fh IRQ */
    if (op == 0x00 || op == 0x01 || op == 0x03 || (op >= 0x04 && op <= 0x1E) || op == 0xE0 || (op >= 0xE1 && op <= 0xE6) || (op >= 0xE7 && op <= 0xEF) || op == 0x1F) return 1;
    if (op == 0x02) return 3;
    if (op >= 0x20 && op <= 0x3F) {
        unsigned verts = (op & 8) ? 4 : 3, per = 1 + ((op & 4) ? 1 : 0), gour = (op & 0x10) ? 1 : 0;
        return 1 + verts * per + (gour ? verts - 1 : 0);
    }
    if (op >= 0x40 && op <= 0x5F) {
        if (op & 8) { x->polyline = 1; return 0; }          /* poly-line: variable, ends with 0x5xxx5xxx */
        return (op & 0x10) ? 4 : 3;
    }
    if (op >= 0x60 && op <= 0x7F) {
        unsigned sz = (op >> 3) & 3;
        return 2 + ((op & 4) ? 1 : 0) + (sz == 0 ? 1 : 0);
    }
    if (op >= 0x80 && op <= 0x9F) return 4;
    if (op >= 0xA0 && op <= 0xDF) return 3;
    return 0xFFFF;                                           /* unknown */
}

/* ---------------------------------------------------------------- rasteriser (queue P4-2) ----
 * Software rasteriser for the GP0 primitives; sources: psx-spx "GPU Rendering Commands" (packet layouts, texture pages, CLUTs, texture
 * window, semi-transparency modes, dithering matrix, mask bit, draw area / offset). Interpolation uses exact integer barycentrics with
 * truncation; it is NOT a bit-exact model of the hardware's incremental interpolators (finding 0025). */
typedef struct { int textured, gouraud, semi, raw; int tpx, tpy, tdepth, tsemi; int clx, cly; } PrimFlags;

static int sx11(uint32_t v) { return ((int)(v & 0x7FF) ^ 0x400) - 0x400; }

static int clip_x1(IoCtx *x) { return (int)(x->draw_tl & 0x3FF); }
static int clip_y1(IoCtx *x) { return (int)((x->draw_tl >> 10) & 0x3FF); }
static int clip_x2(IoCtx *x) { return (int)(x->draw_br & 0x3FF); }
static int clip_y2(IoCtx *x) { return (int)((x->draw_br >> 10) & 0x3FF); }

/* CLUT cache (public ps1-tests gpu/clut-cache): a textured primitive with a 4-bit texture needs the first 16 palette entries, an 8-bit one 256, a 15-bit one none. The cache is
 * filled when it does not hold the same palette (x,y) or holds fewer entries than needed; it is NOT refreshed when VRAM changes (CPU writes, fills, lines, copies) nor when E1 changes
 * the texture mode (nothing happens until something is drawn); GP0(01h) empties it. A palette that was used with 256 entries still serves a later 4-bit primitive. */
static void clut_prepare(IoCtx *x, const PrimFlags *f) {
    if (f->tdepth > 1) return;
    int need = f->tdepth == 0 ? 16 : 256;
    if (x->clut_n >= need && x->clut_x == f->clx && x->clut_y == f->cly) return;
    for (int i = 0; i < need; i++) x->clut_c[i] = x->vram[f->cly * VW + ((f->clx + i) & (VW - 1))];
    x->clut_n = need; x->clut_x = f->clx; x->clut_y = f->cly;
}

static uint16_t tex_fetch(IoCtx *x, const PrimFlags *f, int u, int v) {
    uint32_t tw = x->tw;                                   /* E2: mask x/y (bits 0-9 in 8-pixel units), offset x/y (bits 10-19) */
    int mx = (int)(tw & 0x1F) * 8, my = (int)((tw >> 5) & 0x1F) * 8, ox = (int)((tw >> 10) & 0x1F) * 8, oy = (int)((tw >> 15) & 0x1F) * 8;
    u = (u & ~mx) | (ox & mx);
    v = (v & ~my) | (oy & my);
    u &= 0xFF; v &= 0xFF;
    int by = (f->tpy + v) & (VH - 1);
    if (f->tdepth == 0) {                                  /* 4 bit CLUT */
        uint16_t w = x->vram[by * VW + ((f->tpx + u / 4) & (VW - 1))];
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return x->clut_c[idx];
    }
    if (f->tdepth == 1) {                                  /* 8 bit CLUT */
        uint16_t w = x->vram[by * VW + ((f->tpx + u / 2) & (VW - 1))];
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return x->clut_c[idx];
    }
    return x->vram[by * VW + ((f->tpx + u) & (VW - 1))];  /* 15 bit direct */
}

static const int dither_m[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

/* write one pixel: (r,g,b) 8 bit colour after modulation, `texel_semi` = texel bit 15 for textured primitives */
static void put_pixel(IoCtx *x, const PrimFlags *f, int px, int py, int r, int g, int b, int texel_semi, int shaded) {
    if (px < clip_x1(x) || px > clip_x2(x) || py < clip_y1(x) || py > clip_y2(x)) return;
    if ((x->stat_texpage & 0x400) == 0 && (x->disp_bits & (1u << 22)) && (py & 1) == ((x->stat_texpage >> 31) & 1) && 0) return;
    uint16_t *dst = &x->vram[(py & (VH - 1)) * VW + (px & (VW - 1))];
    if ((x->mask_bits & (2u << 11)) && (*dst & 0x8000)) return;           /* E6.1: do not draw to masked pixels */
    if ((x->stat_texpage & 0x200) && shaded) {                            /* dither enable (E1.9) applies to shaded/modulated pixels */
        int dv = dither_m[py & 3][px & 3];
        r += dv; g += dv; b += dv;
        r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    }
    int nr = r >> 3, ng = g >> 3, nb = b >> 3;
    if (f->semi && (!f->textured || texel_semi)) {
        uint16_t bk = *dst;
        int br = bk & 31, bg = (bk >> 5) & 31, bb = (bk >> 10) & 31;
        switch (f->tsemi) {
        case 0: nr = (br + nr) / 2; ng = (bg + ng) / 2; nb = (bb + nb) / 2; break;
        case 1: nr = br + nr; ng = bg + ng; nb = bb + nb; break;
        case 2: nr = br - nr; ng = bg - ng; nb = bb - nb; break;
        default: nr = br + nr / 4; ng = bg + ng / 4; nb = bb + nb / 4; break;
        }
        nr = nr < 0 ? 0 : nr > 31 ? 31 : nr; ng = ng < 0 ? 0 : ng > 31 ? 31 : ng; nb = nb < 0 ? 0 : nb > 31 ? 31 : nb;
    }
    uint16_t out = (uint16_t)(nr | (ng << 5) | (nb << 10));
    if (x->mask_bits & (1u << 11)) out |= 0x8000;                         /* E6.0: set the mask bit on draw */
    *dst = out;
}

static void shade_texel(IoCtx *x, const PrimFlags *f, int px, int py, int u, int v, int r, int g, int b, int shaded) {
    uint16_t t = tex_fetch(x, f, u, v);
    if (t == 0) return;                                                    /* texel 0000h is transparent */
    int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
    if (!f->raw) {                                                         /* modulation: texel * colour / 128, saturated */
        tr = tr * r >> 7; tg = tg * g >> 7; tb = tb * b >> 7;
        tr = tr > 255 ? 255 : tr; tg = tg > 255 ? 255 : tg; tb = tb > 255 ? 255 : tb;
    }
    put_pixel(x, f, px, py, tr, tg, tb, (t >> 15) & 1, shaded && !f->raw);
}

/* Colour and texture-coordinate interpolation of polygons (finding 0043): attributes are planes in 20-bit fixed point. The gradient per pixel step is the exact plane slope scaled by 4096 and
 * truncated toward zero; the value at a pixel is the attribute of the LEFTMOST vertex (smaller x wins ties between the first two, then the third must be strictly smaller to replace the first) times
 * 4096 plus a half unit (2048), plus the gradients times the distance from that vertex, shifted back by 12 and reduced to 8 bits. Against the public ps1-tests captures this removes the +-1 steps of
 * the previous barycentric rounding (gouraud triangle, uv quads). The origin vertex matters: a different one gives different low bits. */
typedef struct { int64_t base[5], dx[5], dy[5]; int ox, oy; } Plane;
static void plane_setup(Plane *pl, Vtx o0, Vtx o1, Vtx o2, Vtx a, Vtx b, Vtx c, int64_t det, int shaded_rgb, int uv) {
    const Vtx *tl = (o1.x <= o0.x) ? ((o2.x <= o1.x) ? &o2 : &o1) : ((o2.x < o0.x) ? &o2 : &o0);
    int av[3][5] = {{a.r, a.g, a.b, a.u, a.v}, {b.r, b.g, b.b, b.u, b.v}, {c.r, c.g, c.b, c.u, c.v}};
    int tv[5] = {tl->r, tl->g, tl->b, tl->u, tl->v};
    pl->ox = tl->x; pl->oy = tl->y;
    for (int k = 0; k < 5; k++) {
        int active = (k < 3) ? shaded_rgb : uv;
        pl->base[k] = ((int64_t)tv[k] << 12) + 2048;
        if (!active) { pl->dx[k] = pl->dy[k] = 0; continue; }
        int64_t nx = (int64_t)(av[1][k] - av[0][k]) * (c.y - b.y) - (int64_t)(av[2][k] - av[1][k]) * (b.y - a.y);
        int64_t ny = (int64_t)(b.x - a.x) * (av[2][k] - av[1][k]) - (int64_t)(c.x - b.x) * (av[1][k] - av[0][k]);
        pl->dx[k] = nx * 4096 / det;                                   /* C division: truncation toward zero */
        pl->dy[k] = ny * 4096 / det;
    }
}
static inline int plane_at(const Plane *pl, int k, int px, int py) {
    int64_t v = pl->base[k] + pl->dx[k] * (px - pl->ox) + pl->dy[k] * (py - pl->oy);
    return (int)((v >> 12) & 0xFF);
}

static void draw_tri(IoCtx *x, const PrimFlags *f, Vtx a, Vtx b, Vtx c) {
    Vtx o0 = a, o1 = b, o2 = c;                                           /* vertex order as sent: the gradient origin is chosen from it */
    int64_t area = (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(c.x - a.x) * (b.y - a.y);
    if (area == 0) return;
    if (area < 0) { Vtx t = b; b = c; c = t; area = -area; }
    int minx = a.x, maxx = a.x, miny = a.y, maxy = a.y;
    Vtx *p[3] = {&a, &b, &c};
    for (int i = 1; i < 3; i++) { if (p[i]->x < minx) minx = p[i]->x; if (p[i]->x > maxx) maxx = p[i]->x; if (p[i]->y < miny) miny = p[i]->y; if (p[i]->y > maxy) maxy = p[i]->y; }
    if (maxx - minx > 1023 || maxy - miny > 511) return;                  /* the GPU drops primitives larger than this */
    if (minx < clip_x1(x)) minx = clip_x1(x);
    if (maxx > clip_x2(x)) maxx = clip_x2(x);
    if (miny < clip_y1(x)) miny = clip_y1(x);
    if (maxy > clip_y2(x)) maxy = clip_y2(x);
    int top[3], left[3];
    Vtx *e0[3] = {&a, &b, &c}, *e1[3] = {&b, &c, &a};                      /* edges a->b, b->c, c->a */
    for (int i = 0; i < 3; i++) {
        int dx = e1[i]->x - e0[i]->x, dy = e1[i]->y - e0[i]->y;
        top[i] = dy == 0 && dx > 0;
        left[i] = dy < 0;
    }
    int shaded = f->gouraud || f->textured;
    Plane pl;
    int64_t det = (int64_t)(b.x - a.x) * (c.y - b.y) - (int64_t)(c.x - b.x) * (b.y - a.y);   /* same plane determinant as the area (the vertex order may have been swapped above) */
    plane_setup(&pl, o0, o1, o2, a, b, c, det, f->gouraud, f->textured);
    for (int py = miny; py <= maxy; py++) for (int px = minx; px <= maxx; px++) {
        int inside = 1;
        for (int i = 0; i < 3; i++) {
            int64_t w = (int64_t)(e1[i]->x - e0[i]->x) * (py - e0[i]->y) - (int64_t)(e1[i]->y - e0[i]->y) * (px - e0[i]->x);
            if (w < 0 || (w == 0 && !(top[i] || left[i]))) { inside = 0; break; }
        }
        if (!inside) continue;
        int r = a.r, g = a.g, bl = a.b;
        if (f->gouraud) { r = plane_at(&pl, 0, px, py); g = plane_at(&pl, 1, px, py); bl = plane_at(&pl, 2, px, py); }
        else { r = o0.r; g = o0.g; bl = o0.b; }
        if (f->textured) shade_texel(x, f, px, py, plane_at(&pl, 3, px, py), plane_at(&pl, 4, px, py), r, g, bl, shaded);
        else put_pixel(x, f, px, py, r, g, bl, 1, f->gouraud);
    }
}

static void set_flags_from_texpage(IoCtx *x, PrimFlags *f, uint32_t tp) {
    f->tpx = (int)(tp & 0xF) * 64; f->tpy = (int)((tp >> 4) & 1) * 256;
    f->tsemi = (int)((tp >> 5) & 3); f->tdepth = (int)((tp >> 7) & 3); if (f->tdepth == 3) f->tdepth = 2;
    (void)x;
}

/* Line rasteriser (finding 0032): the major axis is stepped in 32-bit fixed point from the pixel centre, the slope is rounded away from zero, colours
 * use 12-bit fixed point with a half-unit start, the end point with the smaller x comes first; k+1 pixels are drawn (k = major axis length). */
static void draw_line(IoCtx *x, const PrimFlags *f, Vtx a, Vtx b) {
    int idx = abs(b.x - a.x), idy = abs(b.y - a.y), k = idx > idy ? idx : idy;
    if (idx >= 1024 || idy >= 512) return;
    if (a.x >= b.x && k > 0) { Vtx t = a; a = b; b = t; }
    int64_t dxdk = 0, dydk = 0; int drdk = 0, dgdk = 0, dbdk = 0;
    if (k) {
        int64_t dx = b.x - a.x, dy = b.y - a.y;
        dxdk = (int64_t)(((dx * ((int64_t)1 << 32)) - (dx < 0 ? k - 1 : 0) + (dx > 0 ? k - 1 : 0)) / k);
        dydk = (int64_t)(((dy * ((int64_t)1 << 32)) - (dy < 0 ? k - 1 : 0) + (dy > 0 ? k - 1 : 0)) / k);
        drdk = ((b.r - a.r) * 4096) / k; dgdk = ((b.g - a.g) * 4096) / k; dbdk = ((b.b - a.b) * 4096) / k;
    }
    int64_t curx = ((int64_t)a.x * ((int64_t)1 << 32)) + ((int64_t)1 << 31) - 1024;
    int64_t cury = ((int64_t)a.y * ((int64_t)1 << 32)) + ((int64_t)1 << 31) - (dydk < 0 ? 1024 : 0);
    int cr = a.r * 4096 + 2048, cg = a.g * 4096 + 2048, cb = a.b * 4096 + 2048;
    for (int i = 0; i <= k; i++) {
        int px = sx11((uint32_t)(int32_t)(curx >> 32)), py = sx11((uint32_t)(int32_t)(cury >> 32));
        int r = f->gouraud ? (uint8_t)(cr >> 12) : a.r, g = f->gouraud ? (uint8_t)(cg >> 12) : a.g, bl = f->gouraud ? (uint8_t)(cb >> 12) : a.b;
        put_pixel(x, f, px, py, r, g, bl, 1, 1);                       /* lines are dithered whether flat or shaded (public ps1-tests capture, finding 0043); flat polygons are not */
        curx += dxdk; cury += dydk; cr += drdk; cg += dgdk; cb += dbdk;
    }
}

static unsigned long long g_gp0_hist[256]; static int g_gp0_hist_on = -1;   /* debug aid: env PS1_GP0HIST=1 prints the count of every drawing opcode when the program ends */
static void gp0_hist_report(void) { for (unsigned i = 0; i < 256; i++) if (g_gp0_hist[i]) fprintf(stderr, "[gp0hist] %02x %llu\n", i, (unsigned long long)g_gp0_hist[i]); }
static void draw_prim(IoCtx *x) {
    uint32_t *c = x->cmd;
    unsigned op = c[0] >> 24;
    if (g_gp0_hist_on < 0) { g_gp0_hist_on = getenv("PS1_GP0HIST") != NULL; if (g_gp0_hist_on) atexit(gp0_hist_report); }
    if (g_gp0_hist_on) g_gp0_hist[op]++;
    int ox = sx11(x->draw_ofs), oy = sx11(x->draw_ofs >> 11);
    PrimFlags f; memset(&f, 0, sizeof f);
    int base_r = c[0] & 0xFF, base_g = (c[0] >> 8) & 0xFF, base_b = (c[0] >> 16) & 0xFF;
    /* texture page of the draw mode (E1) as the default for rectangles */
    uint32_t e1tp = (x->stat_texpage & 0x1FF);
    if (op >= 0x20 && op <= 0x3F) {
        int quad = (op >> 3) & 1, gour = (op >> 4) & 1, tex = (op >> 2) & 1;
        f.textured = tex; f.gouraud = gour; f.semi = (op >> 1) & 1; f.raw = tex ? (int)(op & 1) : 0;
        Vtx v[4]; memset(v, 0, sizeof v);
        unsigned k = 1, nv = quad ? 4 : 3;
        uint32_t clut = 0, tpage = 0;
        for (unsigned i = 0; i < nv; i++) {
            v[i].r = base_r; v[i].g = base_g; v[i].b = base_b;
            if (i > 0 && gour) { uint32_t col = c[k++]; v[i].r = col & 0xFF; v[i].g = (col >> 8) & 0xFF; v[i].b = (col >> 16) & 0xFF; }
            uint32_t xy = c[k++];
            v[i].x = sx11(xy) + ox; v[i].y = sx11(xy >> 16) + oy;
            if (tex) { uint32_t uv = c[k++]; v[i].u = uv & 0xFF; v[i].v = (uv >> 8) & 0xFF; if (i == 0) clut = uv >> 16; if (i == 1) tpage = uv >> 16; }
        }
        if (!tex) set_flags_from_texpage(x, &f, e1tp);                      /* untextured polygons take the semi-transparency mode from the draw mode (E1 / GPUSTAT), not from a texture page */
        if (tex) {
            set_flags_from_texpage(x, &f, tpage);
            f.clx = (int)(clut & 0x3F) * 16; f.cly = (int)((clut >> 6) & 0x1FF);
            clut_prepare(x, &f);
            x->stat_texpage = (x->stat_texpage & ~0x1FFu) | (tpage & 0x1FFu);    /* the polygon's texture page replaces the draw mode's */
            if (x->tex_dis_ok) x->stat_texpage = (x->stat_texpage & ~0x8000u) | ((tpage & 0x800u) << 4);
        }
        draw_tri(x, &f, v[0], v[1], v[2]);
        if (quad) draw_tri(x, &f, v[1], v[2], v[3]);
    } else if (op >= 0x60 && op <= 0x7F) {
        int tex = (op >> 2) & 1, sz = (op >> 3) & 3;
        f.textured = tex; f.semi = (op >> 1) & 1; f.raw = tex ? (int)(op & 1) : 0;
        set_flags_from_texpage(x, &f, e1tp);
        unsigned k = 1;
        uint32_t xy = c[k++];
        int rx = sx11(xy) + ox, ry = sx11(xy >> 16) + oy, u0 = 0, v0 = 0, w, h;
        if (tex) { uint32_t uv = c[k++]; u0 = uv & 0xFF; v0 = (uv >> 8) & 0xFF; f.clx = (int)((uv >> 16) & 0x3F) * 16; f.cly = (int)((uv >> 22) & 0x1FF); clut_prepare(x, &f); }
        if (sz == 0) { uint32_t wh = c[k++]; w = (int)(wh & 0x3FF); h = (int)((wh >> 16) & 0x1FF); }
        else if (sz == 1) { w = h = 1; } else if (sz == 2) { w = h = 8; } else { w = h = 16; }
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int px = rx + i, py = ry + j;
            if (tex) {
                int u = (x->e1_flip & 1) ? ((u0 | 1) - i) & 0xFF : (u0 + i) & 0xFF,         /* X flip starts at u0|1 (the public ps1-tests capture shows 1,0,255,.. for u0 = 0; the |1 form is recalled from another emulator, only u0 = 0 is evidenced, finding 0043); Y flip starts at v0 */
                    v = (x->e1_flip & 2) ? (v0 - j) & 0xFF : (v0 + j) & 0xFF;
                shade_texel(x, &f, px, py, u, v, base_r, base_g, base_b, 0);
            } else put_pixel(x, &f, px, py, base_r, base_g, base_b, 1, 0);
        }
    } else if (op >= 0x40 && op <= 0x5F) {
        f.gouraud = (op >> 4) & 1; f.semi = (op >> 1) & 1;
        set_flags_from_texpage(x, &f, e1tp);
        uint32_t xy0 = c[1], col1 = f.gouraud ? c[2] : c[0], xy1 = f.gouraud ? c[3] : c[2];
        Vtx a = {sx11(xy0) + ox, sx11(xy0 >> 16) + oy, base_r, base_g, base_b, 0, 0};
        Vtx b = {sx11(xy1) + ox, sx11(xy1 >> 16) + oy, (int)(col1 & 0xFF), (int)((col1 >> 8) & 0xFF), (int)((col1 >> 16) & 0xFF), 0, 0};
        draw_line(x, &f, a, b);
    }
}

/* debug aid (queue P1-31): env PS1_GPULOG=1 prints, per executable phase, every distinct value written to the GPU environment commands E1..E6 and display registers */
static char g_phase[24] = "boot";
void io_gpulog_phase(const char *name) { snprintf(g_phase, sizeof g_phase, "%s", name); }
static void gpulog_note(const char *kind, uint64_t v) {
    static struct { char ph[24]; char kind[8]; uint64_t v; } seen[4096];
    static unsigned n;
    static int on = -1;
    if (on < 0) on = getenv("PS1_GPULOG") != NULL;
    if (!on) return;
    for (unsigned i = 0; i < n; i++) if (seen[i].v == v && !strcmp(seen[i].kind, kind) && !strcmp(seen[i].ph, g_phase)) return;
    if (n < 4096) { snprintf(seen[n].ph, 24, "%s", g_phase); snprintf(seen[n].kind, 8, "%s", kind); seen[n++].v = v; }
    fprintf(stderr, "[gpu] %s %s %llx\n", g_phase, kind, (unsigned long long)v);
}

static void exec_cmd(IoCtx *x) {
    uint32_t *c = x->cmd;
    unsigned op = c[0] >> 24;
    COV(RT_COV_GP0, op);
    if (op == 0x02) {                                        /* fill rectangle in VRAM, ignores draw area/mask */
        int fx = c[1] & 0x3F0, fy = (c[1] >> 16) & 0x1FF, w = ((c[2] & 0x3FF) + 0xF) & ~0xF, h = (c[2] >> 16) & 0x1FF;
        uint16_t col = (uint16_t)(((c[0] & 0xF8) >> 3) | ((c[0] & 0xF800) >> 6) | ((c[0] & 0xF80000) >> 9));
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) vram_fill_put(x, fx + i, fy + j, col);
        x->c_fills++;
    } else if (op >= 0x20 && op <= 0x7F) {
        x->c_prims++;
        draw_prim(x);
    } else if (op >= 0x80 && op <= 0x9F) {                   /* VRAM -> VRAM */
        int sx = c[1] & 0x3FF, sy = (c[1] >> 16) & 0x1FF, dx = c[2] & 0x3FF, dy = (c[2] >> 16) & 0x1FF;
        int w = ((c[3] - 1) & 0x3FF) + 1, h = (((c[3] >> 16) - 1) & 0x1FF) + 1;
        for (int j = 0; j < h; j++) {                           /* top to bottom; each row is read completely before it is written (overlapping copies of the public ps1-tests match a real capture this way, finding 0043) */
            uint16_t row[1024];
            for (int i = 0; i < w; i++) row[i] = x->vram[((sy + j) & (VH - 1)) * VW + ((sx + i) & (VW - 1))];
            for (int i = 0; i < w; i++) vram_put(x, dx + i, dy + j, row[i]);
        }
    } else if (op >= 0xA0 && op <= 0xBF) {                   /* CPU -> VRAM */
        x->tx = c[1] & 0x3FF; x->ty = (c[1] >> 16) & 0x1FF;
        x->tw_ = ((c[2] - 1) & 0x3FF) + 1;
        x->th = (((c[2] >> 16) - 1) & 0x1FF) + 1;
        gpulog_note("LoadImg", ((uint64_t)x->tx << 36) | ((uint64_t)x->ty << 24) | ((uint64_t)x->tw_ << 12) | (uint64_t)x->th);   /* PS1_GPULOG: x<<36 | y<<24 | w<<12 | h of every distinct CPU->VRAM upload */
        if (getenv("PS1_UPLOG")) fprintf(stderr, "[up] %u,%u %ux%u\n", x->tx, x->ty, x->tw_, x->th);
        x->cx = x->cy = 0; x->tpos = 0; x->xfer = 1; x->c_uploads++;
    } else if (op >= 0xC0 && op <= 0xDF) {                   /* VRAM -> CPU */
        x->tx = c[1] & 0x3FF; x->ty = (c[1] >> 16) & 0x1FF;
        x->tw_ = ((c[2] - 1) & 0x3FF) + 1;
        x->th = (((c[2] >> 16) - 1) & 0x1FF) + 1;
        x->cx = x->cy = 0; x->tpos = 0; x->xfer = 2;
    } else if (op == 0x01) {
        x->clut_n = 0;                                                       /* clear texture cache */
    } else if (op == 0xE1) {
        gpulog_note("E1", c[0] & 0xFFFFFF);
        x->stat_texpage = (c[0] & 0x7FF) | (x->tex_dis_ok ? ((c[0] & 0x800) << 4) : 0);        /* GPUSTAT bit 15 follows E1 bit 11 only when GP1(09h) allows it, otherwise it is cleared by the write (public ps1-tests gp0-e1, finding 0043) */
        x->e1_flip = (c[0] >> 12) & 3;
    } else if (op == 0xE2) { gpulog_note("E2", c[0] & 0xFFFFFF); x->tw = c[0] & 0xFFFFF; }
    else if (op == 0xE3) { gpulog_note("E3", c[0] & 0xFFFFFF); x->draw_tl = c[0] & 0xFFFFF; }
    else if (op == 0xE4) { gpulog_note("E4", c[0] & 0xFFFFFF); x->draw_br = c[0] & 0xFFFFF; }
    else if (op == 0xE5) { gpulog_note("E5", c[0] & 0xFFFFFF); x->draw_ofs = c[0] & 0x3FFFFF; }
    else if (op == 0xE6) { gpulog_note("E6", c[0] & 0xFFFFFF); x->mask_bits = (c[0] & 3) << 11; }
    else if (op == 0x1F) x->irq1 = 1;
    /* 0x00 NOP, 0x01 clear texture cache: nothing to do */
}

/* poly-line words: [colour+opcode] xy, then for each further vertex ([colour] xy), ended by a word with the 5xxx5xxxh pattern where a colour (shaded) or
 * a vertex (flat) is expected; every new vertex draws a segment from the previous one */
static void polyline_word(IoCtx *x, uint32_t w) {
    int gour = (x->pl_op >> 4) & 1;
    PrimFlags f; memset(&f, 0, sizeof f);
    f.gouraud = gour; f.semi = (x->pl_op >> 1) & 1;
    set_flags_from_texpage(x, &f, x->stat_texpage & 0x1FF);
    int ox = sx11(x->draw_ofs), oy = sx11(x->draw_ofs >> 11);
    switch (x->pl_state) {
    case 1: case 3:                                          /* a colour (shaded) / a vertex (flat) is expected: the terminator can appear here */
        if ((w & 0xF000F000u) == 0x50005000u) { x->pl_active = 0; x->c_prims++; return; }
        if (gour && x->pl_state == 1) { x->pl_r = w & 0xFF; x->pl_g = (w >> 8) & 0xFF; x->pl_b = (w >> 16) & 0xFF; x->pl_state = 2; return; }
        /* a flat vertex: continue into the vertex case */
        /* fall through */
    case 0: case 2: {
        Vtx v = {sx11(w) + ox, sx11(w >> 16) + oy, x->pl_r, x->pl_g, x->pl_b, 0, 0};
        if (x->pl_nv > 0) draw_line(x, &f, x->pl_prev, v);
        x->pl_prev = v; x->pl_nv++;
        x->pl_state = gour ? 1 : 3;
        return;
    }
    }
}

static void gp0_write(IoCtx *x, uint32_t w) {
    x->c_words++;
    if (x->xfer == 1) { xfer_word(x, w); return; }
    if (x->pl_active) { polyline_word(x, w); return; }
    if (x->ncmd == 0) {
        x->need = cmd_len(x, w);
        if (x->need == 0xFFFF) { snprintf(x->err, sizeof x->err, "unknown GP0 command %08x", w); x->need = 1; }
        if (x->polyline) {                                   /* poly-line: the opcode word carries the first colour */
            x->polyline = 0; x->pl_active = 1; x->pl_state = 0; x->pl_op = (int)(w >> 24); x->pl_nv = 0;
            x->pl_r = w & 0xFF; x->pl_g = (w >> 8) & 0xFF; x->pl_b = (w >> 16) & 0xFF;
            return;
        }
    }
    if (x->ncmd < 16) x->cmd[x->ncmd] = w;
    x->ncmd++;
    if (x->ncmd == x->need) { exec_cmd(x); x->ncmd = 0; }
}

static void xfer_word(IoCtx *x, uint32_t w) {
    for (int half = 0; half < 2; half++) {
        uint16_t px = (uint16_t)(w >> (16 * half));
        vram_put(x, x->tx + x->cx, x->ty + x->cy, px);
        if (++x->cx == x->tw_) { x->cx = 0; if (++x->cy == x->th) { x->xfer = 0; return; } }
    }
}

static uint32_t gpuread(IoCtx *x) {
    if (x->xfer == 2) {
        uint32_t w = 0;
        for (int half = 0; half < 2; half++) {
            uint16_t px = x->vram[((x->ty + x->cy) & (VH - 1)) * VW + ((x->tx + x->cx) & (VW - 1))];
            w |= (uint32_t)px << (16 * half);
            if (++x->cx == x->tw_) { x->cx = 0; if (++x->cy == x->th) { x->xfer = 0; break; } }
        }
        x->gpuread = w;
    }
    return x->gpuread;
}

static void gp1_write(IoCtx *x, uint32_t w) {
    unsigned op = w >> 24;
    COV(RT_COV_GP1, op);
    switch (op) {
    case 0x00:                                               /* reset GPU */
        x->stat_texpage = x->mask_bits = x->disp_bits = 0; x->disp_off = 1; x->dma_dir = 0; x->irq1 = 0;
        x->disp_start = 0; x->hrange = 0xC60260; x->vrange = 0x042018; x->draw_tl = x->draw_br = x->draw_ofs = x->tw = 0;
        x->ncmd = 0; x->xfer = 0; x->pl_active = 0; x->tex_dis_ok = 0; break;
    case 0x01: x->ncmd = 0; x->xfer = 0; x->pl_active = 0; break;              /* reset command buffer */
    case 0x02: x->irq1 = 0; break;
    case 0x03: x->disp_off = w & 1; break;
    case 0x04: x->dma_dir = w & 3; break;
    case 0x05: gpulog_note("GP1_05", w & 0xFFFFFF); x->disp_start = w & 0x7FFFF; { static int fl = -1; if (fl < 0) fl = getenv("PS1_FLIPLOG") != NULL; if (fl) fprintf(stderr, "[flip] frame %llu start %05x\n", (unsigned long long)x->frames, w & 0x7FFFF); } break;     /* debug aid: PS1_FLIPLOG=1 prints every change of the display start (buffer swaps) with the virtual frame */
    case 0x06: gpulog_note("GP1_06", w & 0xFFFFFF); x->hrange = w & 0xFFFFFF; break;
    case 0x07: gpulog_note("GP1_07", w & 0xFFFFFF); x->vrange = w & 0xFFFFF; break;
    case 0x09: x->tex_dis_ok = w & 1; break;                                    /* allow texture disable (GPU type 2): only gates the write of GPUSTAT bit 15, a stored bit survives it */
    case 0x08: gpulog_note("GP1_08", w & 0xFFFFFF); x->disp_bits = ((w & 3) << 17) | (((w >> 2) & 1) << 19) | (((w >> 3) & 1) << 20) | (((w >> 4) & 1) << 21) |
                              (((w >> 5) & 1) << 22) | (((w >> 6) & 1) << 16) | (((w >> 7) & 1) << 14); break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (w & 0xF) {                                   /* GPU info: only the registers the game side can read back from state we hold */
        case 2: x->gpuread = x->tw; break;
        case 3: x->gpuread = x->draw_tl; break;
        case 4: x->gpuread = x->draw_br; break;
        case 5: x->gpuread = x->draw_ofs; break;
        case 7: x->gpuread = 2; break;                       /* GPU version: psx-spx "GP1(10h) type 7 = 2 for the standard GPU"; unverified for this game */
        default: snprintf(x->err, sizeof x->err, "GP1(%02xh) info type %u unproven", op, w & 0xF); break;
        }
        break;
    default: snprintf(x->err, sizeof x->err, "GP1 command %08x unproven", w); break;
    }
}

/* ---------------------------------------------------------------- DMA ---- */
static void dicr_update(IoCtx *x, int ch) {
    uint32_t en = (x->dicr >> 16) & 0x7F;
    if (en & (1u << ch)) x->dicr |= 1u << (24 + ch);
    uint32_t flags = (x->dicr >> 24) & 0x7F;
    uint32_t old = x->dicr >> 31;
    uint32_t master = ((x->dicr >> 15) & 1) | (((x->dicr >> 23) & 1) & ((flags & en) != 0));
    x->dicr = (x->dicr & 0x7FFFFFFFu) | (master << 31);
    if (!old && master) { x->i_stat |= 1u << 3; COV(RT_COV_IRQ, 3); }
}

static int dma_start(IoCtx *x, int n) {
    Chan *c = &x->ch[n];
    COV(RT_COV_DMA, n);
    uint32_t sync = (c->chcr >> 9) & 3, from_ram = c->chcr & 1;
    if (getenv("PS1_DMALOG")) fprintf(stderr, "[dma] frame %llu start channel %d chcr %08x bcr %08x madr %06x dpcr %08x\n", (unsigned long long)x->frames, n, c->chcr, c->bcr, c->madr, x->dpcr);
    if (!((x->dpcr >> (4 * n + 3)) & 1)) return 1;          /* master-disabled: the channel stays busy (CHCR bit 24 set) and runs when DPCR enables it (DuckStation dma.cpp CanTransferChannel; psx-spx DPCR) */
    uint32_t a = c->madr & 0x1FFFFC;
    if (n == 2) {
        if (sync == 2 && from_ram) {                         /* linked list into GP0 */
            for (unsigned guard = 0; guard < 0x100000; guard++) {
                uint32_t h; memcpy(&h, x->ram + a, 4);
                unsigned len = h >> 24;
                if (getenv("PS1_DMALOG")) { fprintf(stderr, "[dma2] node %06x hdr %08x:", a, h); for (unsigned k = 0; k < len && k < 10; k++) { uint32_t w; memcpy(&w, x->ram + ((a + 4 + 4 * k) & 0x1FFFFC), 4); fprintf(stderr, " %08x", w); } fprintf(stderr, "\n"); }
                for (unsigned k = 0; k < len; k++) { uint32_t w; memcpy(&w, x->ram + ((a + 4 + 4 * k) & 0x1FFFFC), 4); gp0_write(x, w); }
                x->c_ot++;
                if (h & 0x800000) break;                     /* end marker: bit 23 of the next pointer (psx-spx: usually FFFFFFh) */
                a = h & 0x1FFFFC;
            }
            x->c_dma2++;
        } else if (sync == 1) {                              /* block mode, RAM <-> GPU */
            uint32_t bs = c->bcr & 0xFFFF, bc = c->bcr >> 16, words = bs * bc;
            for (uint32_t k = 0; k < words; k++, a = (a + 4) & 0x1FFFFC) {
                if (from_ram) { uint32_t w; memcpy(&w, x->ram + a, 4); gp0_write(x, w); }
                else { uint32_t w = gpuread(x); memcpy(x->ram + a, &w, 4); }
            }
            x->c_dma2++;
        } else { snprintf(x->err, sizeof x->err, "DMA2 mode sync=%u dir=%u unproven", sync, from_ram); return 0; }
    } else if (n == 0 || n == 1) {                           /* MDEC input (RAM -> MDEC) / output (MDEC -> RAM), block mode */
        uint32_t words = (c->bcr & 0xFFFF) * (sync == 1 ? (c->bcr >> 16) : 1);
        if ((n == 0) != (from_ram != 0)) { snprintf(x->err, sizeof x->err, "DMA%d direction unproven", n); return 0; }
        if (n == 1) {                                        /* MDEC -> RAM: only as many words as the decoder has produced; the rest waits (a real transfer stalls until data arrives) */
            uint32_t avail = (uint32_t)((x->md.nout - x->md.outpos) / 4), take = words < avail ? words : avail;
            for (uint32_t k = 0; k < take; k++, a = (a + 4) & 0x1FFFFC) { uint32_t w = mdec_read_word(x); memcpy(x->ram + a, &w, 4); }
            if (take < words) { x->d1_pend = 1; x->d1_madr = a; x->d1_left = words - take; return 1; }      /* CHCR keeps its busy bit, no completion interrupt yet */
        } else {                                             /* RAM -> MDEC: the decoder accepts new input only after its output has been read (a real transfer stalls on the full output FIFO) */
            if (x->md.outpos < x->md.nout) { x->d0_pend = 1; x->d0_madr = a; x->d0_left = words; return 1; }
            for (uint32_t k = 0; k < words; k++, a = (a + 4) & 0x1FFFFC) { uint32_t w; memcpy(&w, x->ram + a, 4); mdec_write_word(x, w); }
            if (x->md.outpos < x->md.nout) { x->d0_hold = 1; return 1; }      /* decoded output waits to be read: the transfer stays busy */
        }
    } else if (n == 3) {                                     /* CD-ROM data FIFO -> RAM */
        Cd *cd = &x->cd;
        uint32_t words = (sync == 0) ? (c->bcr & 0xFFFF) : (c->bcr & 0xFFFF) * (c->bcr >> 16);
        if (from_ram) { snprintf(x->err, sizeof x->err, "DMA3 direction RAM->CD unproven"); return 0; }
        for (uint32_t k = 0; k < words; k++, a = (a + 4) & 0x1FFFFC) {
            uint32_t w = 0;
            for (int b = 0; b < 4; b++) w |= (uint32_t)cd_read_reg(x, 0x1F801802) << (8 * b);
            memcpy(x->ram + a, &w, 4);
        }
        (void)cd;
        if (getenv("PS1_DMALOG")) { uint32_t b = c->madr & 0x1FFFFC; fprintf(stderr, "[dma3] %u words to %06x:", words, b); for (unsigned k = 0; k < 20 && k < words * 4; k++) fprintf(stderr, " %02x", x->ram[b + k]); fprintf(stderr, "\n"); }
    } else if (n == 4) {                                     /* SPU: block mode, RAM <-> sound RAM at the transfer address */
        uint32_t bs = c->bcr & 0xFFFF, bc = c->bcr >> 16, words = bs * bc;
        if (sync == 0) words = bs ? bs : 0x10000;                  /* start-immediately mode: BCR is the word count (public ps1-tests spu/memory-transfer, finding 0043) */
        else if (sync != 1) { snprintf(x->err, sizeof x->err, "DMA4 sync mode %u unproven", sync); return 0; }
        for (uint32_t k = 0; k < words; k++, a = (a + 4) & 0x1FFFFC) {
            uint32_t sa = x->spu_addr & 0x7FFFC;
            if (from_ram) memcpy(x->spuram + sa, x->ram + a, 4); else memcpy(x->ram + a, x->spuram + sa, 4);
            x->spu_addr = (x->spu_addr + 4) & 0x7FFFF;
        }
    } else if (n == 6) {                                     /* OTC: ordering table clear, reverse */
        uint32_t words = c->bcr & 0xFFFF; if (words == 0) words = 0x10000;
        for (uint32_t k = 0; k < words; k++, a = (a - 4) & 0x1FFFFC) {
            uint32_t v = (k == words - 1) ? 0x00FFFFFFu : ((a - 4) & 0x1FFFFC);
            memcpy(x->ram + a, &v, 4);
        }
    } else { snprintf(x->err, sizeof x->err, "DMA channel %d transfer unproven", n); return 0; }
    c->chcr &= ~((1u << 24) | (1u << 28));
    x->dma_pend |= 1u << n;
    if (n == 1) mdec_dma0_resume(x);                                 /* the data moved at once but the completion interrupt comes later (see io_dma_irq_next) */
    return 1;
}

int io_irq_pending(IoCtx *x) { return (x->i_stat & x->i_mask) != 0; }

/* One video frame passes (host-owned time): timer 1 counts the scanlines of a frame (psx-spx: NTSC 263, PAL 313, from GPUSTAT bit 20),
 * the vblank bit of I_STAT is raised. Not called by the register model itself; the HLE of VSync and, later, the frame loop call it. */
/* Timer 2 follows its mode register (psx-spx root counters): clock = sysclk (mode bits 8-9 = 0/1) or sysclk/8 (2/3), 564480 system clocks per NTSC frame; sync modes 0 and 3
 * stop it; with bit 3 the counter counts 0..target and resets (period target+1), otherwise it wraps at 0xFFFF; IRQ on target (bit 4) and/or overflow (bit 5), repeating (bit 6) or one
 * shot. Interrupts are queued in t_pend and delivered by io_timer_irq_next (several per frame are possible: a 100 Hz tick is 1.67 per frame). Found because the game's sound
 * sequencer is driven by this interrupt (finding 0040). */
static void timer2_step(IoCtx *x) {
    uint32_t m = x->t_mode[2];
    if ((m & 1) && (((m >> 1) & 3) == 0 || ((m >> 1) & 3) == 3)) return;
    int64_t T = (int64_t)(x->t_target[2] & 0xFFFF), v0 = (int64_t)(x->t_value[2] & 0xFFFF), inc = 564480 / (((m >> 8) & 3) >= 2 ? 8 : 1), tot = v0 + inc, hits, ovf = 0, v;
    if (m & 8) { hits = tot / (T + 1); v = tot % (T + 1); }
    else { ovf = tot / 65536; v = tot % 65536; hits = (tot - T + 65536) / 65536 - (v0 - T + 65536) / 65536; }
    x->t_value[2] = (uint32_t)v;
    int64_t n = ((m & 0x10) ? hits : 0) + ((m & 0x20) ? ovf : 0);
    if (!(m & 0x40)) { if (x->t_done[2]) n = 0; else if (n > 0) { n = 1; x->t_done[2] = 1; } }
    if (n > 8) n = 8;
    x->t_pend[2] += (uint32_t)n;
}
int io_timer_irq_next(IoCtx *x) {
    for (int t = 0; t < 3; t++) if (x->t_pend[t]) { x->t_pend[t]--; x->i_stat |= 1u << (4 + t); COV(RT_COV_IRQ, 4 + t); return t + 1; }
    return 0;
}
uint32_t io_imask(IoCtx *x) { return x->i_mask; }
void io_irq_ack(IoCtx *x, uint32_t bits) { x->i_stat &= ~bits; }

void io_vblank_tick(IoCtx *x) {
    x->t_sub[0] = x->t_sub[1] = x->t_sub[2] = 0;
    x->t_value[1] = (x->t_value[1] + ((x->disp_bits >> 20) & 1 ? 313 : 263)) & 0xFFFF;
    timer2_step(x);
    x->i_stat |= 1u; COV(RT_COV_IRQ, 0);
    x->frames++;
    x->cd.tick++;
    if (x->cd.reading && x->cd.tick >= x->cd.next_sector) {          /* data rate: 75 sectors/s (single) or 150 (double speed) = 1.25 or 2.5 sectors per 60 Hz frame, in quarter sectors */
        x->cd.credit += (x->cd.mode & 0x80) ? 10 : 5;
        if (x->cd.credit > 12) x->cd.credit = 12;
    }
    cd_fire(x);
    spu_frame(x);
}

/* --------------------------------------------------------- dispatcher ---- */
static unsigned long long levpoll_nz[24], levpoll_tot[24];
static IoCtx *levpoll_x;
static void levpoll_report(void) { if (levpoll_x) for (int i = 0; i < 24; i++) { Voice *v = &levpoll_x->spu.v[i]; if (v->level) fprintf(stderr, "[levpoll] v%d on=%d phase=%d level=%d adsr %04x %04x cnt=%d\n", i, v->on, v->phase, v->level, levpoll_x->spu_r[(0x1C08 + 16 * i - 0x1C00) >> 1], levpoll_x->spu_r[(0x1C0A + 16 * i - 0x1C00) >> 1], v->adsr_cnt); }
 fprintf(stderr, "[levpoll] non-zero/total per voice:"); for (int i = 0; i < 24; i++) fprintf(stderr, " %llu/%llu", levpoll_nz[i], levpoll_tot[i]); fprintf(stderr, "\n"); }
static unsigned long long g_io_rd[0x800]; static int g_io_rd_on = -1;   /* debug aid: env PS1_IOHIST=1 prints the most read I/O registers (word index from 0x1F801000) when the program ends */
static void io_hist_report(void) {
    for (int n = 0; n < 40; n++) {
        int best = -1; for (int i = 0; i < 0x800; i++) if (g_io_rd[i] && (best < 0 || g_io_rd[i] > g_io_rd[best])) best = i;
        if (best < 0) break;
        fprintf(stderr, "[iohist] %08x read %llu times\n", 0x1F801000u + 4u * (unsigned)best, g_io_rd[best]); g_io_rd[best] = 0;
    }
}
/* ---------------------------------------------------------------- SIO0 ---- */
/* Serial port 0 (psx-spx "Serial Port (SIO) / Controllers and Memory Cards"): registers DATA 1F801040h, STAT 44h, MODE 48h, CTRL 4Ah, BAUD 4Eh. Only a digital controller (id 41h) in port 1 answers;
 * port 2 and memory-card selections (first byte 81h) do not answer (RX byte FFh, no ACK). One transfer = one byte exchanged at once; the ACK interrupt (I_STAT bit 7) is queued and raised by
 * io_sio_irq_next between the program's handler runs, like the DMA completion interrupts. Pad bytes: 01h (address) -> FFh, 42h (poll) -> 41h, then 5Ah, buttons low, buttons high. */
static void sio_commit(IoCtx *x) {                              /* the byte exchanged earlier arrives: RX data, STAT bit 9 and I_STAT bit 7 (when the ACK interrupt is enabled) */
    if (!x->sio.busy) return;
    x->sio.busy = 0;
    if (x->sio.nrx < 8) x->sio.rx[x->sio.nrx++] = x->sio.p_rx;
    if (x->sio.p_ack && (x->sio.ctrl & 0x1000)) { x->sio.irq_flag = 1; x->i_stat |= 1u << 7; COV(RT_COV_IRQ, 7); }
}
static void sio_exchange(IoCtx *x, uint8_t tx) {
    sio_commit(x);
    uint8_t rx = 0xFF; int ack = 0;
    int selected = (x->sio.ctrl & 2) != 0, port2 = (x->sio.ctrl >> 13) & 1;
    if (selected && !port2) switch (x->sio.state) {
    case 0: if (tx == 0x01) { x->sio.state = 1; ack = 1; } break;                            /* controller address */
    case 1: rx = 0x41; x->sio.state = 2; ack = 1; break;                                      /* command (42h = poll; others are answered like a poll): id low = digital pad */
    case 2: rx = 0x5A; x->sio.state = 3; ack = 1; break;                                      /* id high */
    case 3: rx = (uint8_t)x->sio.pad; x->sio.state = 4; ack = 1; break;                        /* buttons, low byte (select, l3, r3, start, up, right, down, left) */
    case 4: rx = (uint8_t)(x->sio.pad >> 8); x->sio.state = 0; ack = 0; break;                 /* buttons, high byte; the last byte is not acknowledged */
    }
    { static int lg = -1; if (lg < 0) lg = getenv("PS1_SIOLOG") != NULL; if (lg) fprintf(stderr, "[sio] tick %llu tx %02x -> rx %02x (state %d, ctrl %04x, ack %d)\n", (unsigned long long)x->frames, tx, rx, x->sio.state, x->sio.ctrl, ack); }       /* debug aid */
    /* the answer is not there yet: a real transfer takes about a thousand CPU cycles and the ACK pulse follows it. The program clears I_STAT bit 7 and tests STAT right after writing the
     * byte, so the result must appear a few register reads later (3 STAT reads, a DATA read, or the end of the virtual frame), not at once (finding 0006). */
    x->sio.p_rx = rx; x->sio.p_ack = ack; x->sio.busy = 3;
}
int io_sio_irq_next(IoCtx *x) {                                 /* end of the virtual frame: a pending exchange completes (an interrupt-driven driver does not poll STAT) */
    if (!x->sio.busy) return 0;
    uint32_t before = x->i_stat; sio_commit(x);
    return x->i_stat != before;
}
void io_sio_set_pad(IoCtx *x, uint32_t bios_word) {             /* B(16h) format (active low, byte order reversed) -> controller bytes */
    x->sio.pad = (uint16_t)(((bios_word >> 8) & 0xFF) | ((bios_word & 0xFF) << 8));
}
/* Serial port 1 (0x1F801050..5F): the debug UART some SDKs print to. Modelled as a sink: transmitter always ready, nothing received; every transmitted byte goes to the console capture
 * (io_serial_hook). Mode/control/baud registers are plain storage, accepted at any width. */
void (*io_serial_hook)(int ch);
static uint32_t g_sio1[4];
static uint32_t sio1_read(IoCtx *x, uint32_t p, unsigned size) {
    (void)x; (void)size;
    uint32_t reg = (p >> 2) & 3, sh = (p & 3) * 8;
    uint32_t v = reg == 1 ? 0x5u : g_sio1[reg];                       /* STAT: TX ready (bit 0) and TX idle (bit 2); no RX data */
    return v >> sh;
}
static void sio1_write(IoCtx *x, uint32_t p, unsigned size, uint32_t v) {
    (void)x;
    uint32_t reg = (p >> 2) & 3, sh = (p & 3) * 8;
    if (reg == 0) { if (io_serial_hook) io_serial_hook((int)(v & 0xFF)); return; }
    uint32_t mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u) << sh;
    g_sio1[reg] = (g_sio1[reg] & ~mask) | ((v << sh) & mask);
}
static uint32_t sio_read(IoCtx *x, uint32_t p, unsigned size, int *ok) {
    uint32_t reg = p & ~3u, sh = (p & 3) * 8, v = 0;
    if (reg == 0x1F801040) {
        sio_commit(x);
        if (x->sio.nrx) { v = x->sio.rx[0]; memmove(x->sio.rx, x->sio.rx + 1, 7); x->sio.nrx--; } else v = 0xFF;
        return v;
    }
    if (reg == 0x1F801044) {
        if (x->sio.busy && --x->sio.busy == 0) { x->sio.busy = 1; sio_commit(x); }
        v = 1u | (x->sio.nrx ? 2u : 0u) | 4u | (x->sio.irq_flag << 9);
    }
    else if (reg == 0x1F801048) v = x->sio.mode | ((uint32_t)x->sio.ctrl << 16);
    else if (reg == 0x1F80104C) v = (uint32_t)x->sio.baud << 16;
    else { *ok = bad(x, "SIO0 read at %08x (%u bytes) unproven", p, size); return 0; }
    return v >> sh;
}
static void sio_write(IoCtx *x, uint32_t p, unsigned size, uint32_t v, int *ok) {
    uint32_t reg = p & ~3u;
    if (reg == 0x1F801040) { if (x->sio.ctrl & 1) sio_exchange(x, (uint8_t)v); return; }
    uint32_t sh = (p & 3) * 8, mask = (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu) << sh, nv = (v << sh) & mask;
    if (reg == 0x1F801048) {
        uint32_t cur = x->sio.mode | ((uint32_t)x->sio.ctrl << 16);
        cur = (cur & ~mask) | nv;
        x->sio.mode = (uint16_t)cur;
        if (mask >> 16) {
            uint16_t c = (uint16_t)(cur >> 16);
            if (c & 0x40) { x->sio.ctrl = 0; x->sio.nrx = 0; x->sio.state = 0; x->sio.irq_pend = 0; x->sio.irq_flag = 0; x->sio.busy = 0; return; }       /* reset */
            if (c & 0x10) x->sio.irq_flag = 0;                                                  /* acknowledge */
            if (!(c & 2)) { x->sio.state = 0; x->sio.busy = 0; }                                /* deselect ends the transaction */
            x->sio.ctrl = c & ~0x50u;
        }
    } else if (reg == 0x1F80104C) { x->sio.baud = (uint16_t)((((uint32_t)x->sio.baud << 16) & ~mask) >> 16 | (nv >> 16)); }
    else *ok = bad(x, "SIO0 write at %08x (%u bytes) unproven", p, size);
}

/* A program that spins on a root counter ("wait 400 counts") would otherwise see the same value until the next virtual frame, because time only moves in io_vblank_tick: every read of
 * the counter value moves it on by roughly the time a polling loop iteration takes (about 12 CPU cycles), saturating below one frame. The next io_vblank_tick starts from the base value
 * again, so frame-to-frame time is unchanged (finding 0007). */
static uint32_t timer_value(IoCtx *x, int t) {
    uint32_t m = x->t_mode[t], div8 = (t == 2 && ((m >> 8) & 3) >= 2), step = div8 ? 2 : 12, frame = div8 ? 70560 : 564480;
    if (x->t_sub[t] + step < frame) x->t_sub[t] += step;
    uint32_t v = (x->t_value[t] + x->t_sub[t]);
    if (m & 8) { uint32_t T = x->t_target[t] & 0xFFFF; v %= (T + 1); } else v &= 0xFFFF;
    return v & 0xFFFF;
}
uint32_t io_read(IoCtx *x, uint32_t p, unsigned size, int *ok) {
    if (rt_cov_on && p >= 0x1F801000 && p < 0x1F803000) rt_cov(RT_COV_IO_R, (p - 0x1F801000) >> 3);
    *ok = 1;
    if (g_io_rd_on < 0) { g_io_rd_on = getenv("PS1_IOHIST") != NULL; if (g_io_rd_on) atexit(io_hist_report); }
    if (g_io_rd_on && p >= 0x1F801000 && p < 0x1F803000) g_io_rd[(p - 0x1F801000) >> 2]++;
    uint32_t sh = (p & 3) * 8, base = p & ~3u;
    uint32_t v;
    if (p >= 0x1F801040 && p < 0x1F801050) return sio_read(x, p, size, ok);
    if (p >= 0x1F801050 && p < 0x1F801060) return sio1_read(x, p, size);
    if (base == 0x1F801820) { if (size != 4) { *ok = bad(x, "MDEC data read of %u bytes unproven", size, 0); return 0; } return mdec_read_word(x); }
    if (base == 0x1F801824) { if (size != 4) { *ok = bad(x, "MDEC status read of %u bytes unproven", size, 0); return 0; } return mdec_status(x); }
    if (p >= 0x1F801800 && p <= 0x1F801803) { uint32_t b = cd_read_reg(x, p); return size == 4 ? b * 0x01010101u : b; }
    if (base >= 0x1F801000 && base <= 0x1F801020) v = x->memctl[(base - 0x1F801000) >> 2];
    else if (base == 0x1F801810) v = gpuread(x);
    else if (base == 0x1F801814) v = gpustat(x);
    else if (base == 0x1F801070) v = x->i_stat;
    else if (base == 0x1F801074) v = x->i_mask;
    else if (base == 0x1F8010F0) v = x->dpcr;
    else if (base == 0x1F8010F4) v = x->dicr;
    else if (base >= 0x1F801100 && base < 0x1F801130 && (base & 0xF) <= 8) { int t = (base - 0x1F801100) >> 4; unsigned r = (base & 0xF) >> 2; v = r == 0 ? timer_value(x, t) : r == 1 ? x->t_mode[t] : x->t_target[t]; }
    else if (p >= 0x1F801E00 && p < 0x1F801E60) {            /* current voice volume L/R (psx-spx): the model applies volumes at once, so it is the register's effective value */
        if (size == 4 && !(p & 3)) { uint32_t lo = io_read(x, p, 2, ok); if (!*ok) return 0; return lo | (io_read(x, p + 2, 2, ok) << 16); }
        if (size != 2 || (p & 1)) { *ok = bad(x, "SPU read at %08x (%u bytes): only halfword and word registers are proven used", p, size); return 0; }
        int n = (int)(p - 0x1F801E00) >> 2;
        return (uint32_t)(uint16_t)fixvol(x->spu_r[(0x1C00 + 0x10 * n + ((p & 2) ? 2 : 0) - 0x1C00) >> 1]);
    }
    else if (p >= 0x1F801C00 && p < 0x1F801E00) {
        if (size == 1 || (size == 4 && !(p & 3)) || (size == 2 && (p & 1))) {      /* byte / word reads are built from the halfword registers (the bus is 16 bit wide; observed: the FF9 sound driver reads a voice register as a byte) */
            if (size == 2) { *ok = bad(x, "SPU read at %08x (%u bytes): misaligned halfword", p, size); return 0; }
            uint32_t lo = io_read(x, p & ~1u, 2, ok); if (!*ok) return 0;
            if (size == 1) return (lo >> ((p & 1) * 8)) & 0xFF;
            uint32_t hi = io_read(x, p + 2, 2, ok); if (!*ok) return 0;
            return lo | (hi << 16);
        }
        if (size != 2) { *ok = bad(x, "SPU read at %08x (%u bytes): unsupported", p, size); return 0; }
        if (p < 0x1F801D80 && (p & 0xF) == 0xC) {                                       /* current ADSR volume */
            static unsigned long long nz[24], tot[24]; static int lv = -1;
            int vn = (int)((p - 0x1F801C00) >> 4);
            if (lv < 0) lv = getenv("PS1_LEVPOLL") != NULL;                             /* debug aid: at exit print, per voice, how many of the game's level polls read non-zero */
            if (lv) { static int reg; if (!reg) { reg = 1; levpoll_x = x; atexit(levpoll_report); } levpoll_nz[vn] += x->spu.v[vn].level != 0; levpoll_tot[vn]++; (void)nz; (void)tot; }
            if (x->spu.v[vn].on && x->spu.v[vn].phase == 0 && x->spu.v[vn].level == 0) spu_adsr_step(x, vn, &x->spu.v[vn]);     /* a voice keyed on moments ago has already left level 0 on hardware (the program runs thousands of cycles between its key-on and its level poll, but this model steps the SPU once per frame slice): the voice allocator picks a free voice by reading this register (finding 0011) */
            return (uint32_t)x->spu.v[vn].level;
        }
        if (p == 0x1F801DB8 || p == 0x1F801DBA) return (uint32_t)(uint16_t)fixvol(x->spu_r[(0x1D80 + (p & 2) - 0x1C00) >> 1]);   /* current main volume */
        if (p == 0x1F801D9C) return x->spu.endx & 0xFFFF;
        if (p == 0x1F801D9E) return (x->spu.endx >> 16) & 0xFF;
        return p == 0x1F801DAE ? ((x->spu_r[(0x1F801DAA - 0x1F801C00) >> 1] & 0x3F) | (x->spu_irqflag ? 0x40 : 0)) : x->spu_r[(p - 0x1F801C00) >> 1];   /* SPUSTAT: bits 0-5 mirror SPUCNT (psx-spx), the rest reads 0 */
    }
    else if (base >= 0x1F801080 && base < 0x1F8010F0) {
        int n = (base - 0x1F801080) >> 4, r = (base >> 2) & 3;
        if ((n != 0 && n != 1 && n != 2 && n != 3 && n != 4 && n != 6)) { *ok = bad(x, "read of DMA channel register %08x (%u bytes): only channels 0, 1, 2, 3, 4 and 6 are proven used", p, size); return 0; }
        v = r == 0 ? x->ch[n].madr : r == 1 ? x->ch[n].bcr : r == 2 ? x->ch[n].chcr : 0;
        if (r == 3) { *ok = bad(x, "read of unknown DMA register %08x (%u bytes)", p, size); return 0; }
    } else { *ok = bad(x, "read of unproven I/O register %08x (%u bytes)", p, size); return 0; }
    v >>= sh;
    return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
}

void io_write(IoCtx *x, uint32_t p, unsigned size, uint32_t v, int *ok) {
    if (rt_cov_on && p >= 0x1F801000 && p < 0x1F803000) rt_cov(RT_COV_IO_W, (p - 0x1F801000) >> 3);
    *ok = 1;
    uint32_t base = p & ~3u;
    if (p >= 0x1F801040 && p < 0x1F801050) { sio_write(x, p, size, v, ok); return; }
    if (p >= 0x1F801050 && p < 0x1F801060) { sio1_write(x, p, size, v); return; }
    if (base == 0x1F801820 && size == 4) { mdec_write_word(x, v); return; }
    if (base == 0x1F801824 && size == 4) {
        if (v & 0x80000000u) { Mdec *m = &x->md; m->state = 0; m->remain = 0; m->nout = m->outpos = 0; m->status_cmd = 0x00040000u & 0; x->d0_pend = x->d1_pend = x->d0_hold = 0; }      /* reset drops the waiting transfers too */
        x->md.en_in = (int)((v >> 30) & 1); x->md.en_out = (int)((v >> 29) & 1);
        return;
    }
    if (p >= 0x1F801800 && p <= 0x1F801803) { if (size != 1) { *ok = bad(x, "CD register write of %u bytes at %08x unproven", size, p); return; } cd_write_reg(x, p, v); return; }
    if (p >= 0x1F801C00 && p < 0x1F801E00) {
        if (size == 4 && !(p & 3)) { io_write(x, p, 2, v & 0xFFFF, ok); if (*ok) io_write(x, p + 2, 2, v >> 16, ok); return; }      /* a word access is two halfword accesses, low half first (psx-spx) */
        if (size != 2 || (p & 1)) { *ok = bad(x, "SPU write at %08x (%u bytes): only halfword and word registers are proven used", p, size); return; }
        x->spu_r[(p - 0x1F801C00) >> 1] = (uint16_t)v; x->c_spu_w++;
        { static int vl = -1; if (vl < 0) vl = getenv("PS1_SPUVOL") ? atoi(getenv("PS1_SPUVOL")) : -2; if (vl >= 0 && p < 0x1F801D80 && ((p - 0x1F801C00) >> 4) == (uint32_t)vl && (p & 0xF) < 4) fprintf(stderr, "[spuvol] frame %llu v%d reg %x <- %04x\n", (unsigned long long)x->frames, vl, (unsigned)(p & 0xF), (unsigned)(uint16_t)v); }      /* debug aid: PS1_SPUVOL=N logs the volume register writes of voice N */
        if (p >= 0x1F801D80) { char k[12]; snprintf(k, sizeof k, "SPU%04x", p & 0xFFFF); gpulog_note(k, (uint16_t)v); }      /* PS1_GPULOG also lists the SPU control/reverb register values */
        else if (p == 0x1F801C04 || p == 0x1F801C06) { /* voice pitch/start: not listed (too many) */ }
        if (p == 0x1F801D88 || p == 0x1F801D8A) {                                   /* KON */
            uint32_t bits = (uint32_t)(uint16_t)v << (p == 0x1F801D8A ? 16 : 0);
            static int dbg = -1; if (dbg < 0) dbg = getenv("PS1_SPUKON") != NULL;       /* debug aid: one line per key-on with the voice registers (frame, voice, start address, pitch, volume L/R, ADSR) and whether SPUCNT enables the unit */
            if (dbg && getenv("PS1_REFLEV")) { fprintf(stderr, "[ourlev] frame %llu:", (unsigned long long)x->frames); for (int q = 0; q < 24; q++) fprintf(stderr, " %x", x->spu.v[q].level); fprintf(stderr, "\n"); }
            if (dbg) for (int n = 0; n < 24; n++) if (bits >> n & 1) { const uint16_t *r = &x->spu_r[n * 8]; fprintf(stderr, "[kon] frame %llu v%d en=%d vol %04x/%04x pitch %04x start %04x adsr %04x%04x\n", (unsigned long long)x->frames, n, (x->spu_r[(0x1F801DAA - 0x1F801C00) >> 1] >> 15) & 1, r[0], r[1], r[2], r[3], r[5], r[4]); }
            if (x->spu_r[(0x1F801DAA - 0x1F801C00) >> 1] & 0x8000) for (int n = 0; n < 24; n++) if (bits >> n & 1) { COV(RT_COV_SPU_KEYON, n); spu_keyon(x, n); }
        } else if (p == 0x1F801D8C || p == 0x1F801D8E) {                           /* KOFF */
            uint32_t bits = (uint32_t)(uint16_t)v << (p == 0x1F801D8E ? 16 : 0);
            static int kd = -1; if (kd < 0) kd = getenv("PS1_SPUKON") != NULL;
            if (kd) for (int n = 0; n < 24; n++) if (bits >> n & 1) fprintf(stderr, "[koff] frame %llu v%d\n", (unsigned long long)x->frames, n);
            for (int n = 0; n < 24; n++) if ((bits >> n & 1) && x->spu.v[n].on) { x->spu.v[n].phase = 3; x->spu.v[n].adsr_cnt = 0; }     /* release starts at once: the running rate counter of the previous phase (a sustain with shift 31 counts for ~1M samples) must not delay it (finding 0045) */
        } else if (p == 0x1F801DA2) x->spu.rev_addr = (uint32_t)(uint16_t)v * 8;
        if (p < 0x1F801D80 && (p & 0xF) == 0xE) { Voice *rv = &x->spu.v[(p - 0x1F801C00) >> 4]; rv->rep = (((uint32_t)(uint16_t)v & ~1u) << 3) & 0x7FFF0; rv->rep_valid = 1; }      /* ADPCM repeat address register */
        if (getenv("PS1_SPUIRQ") && (p == 0x1F801DA4 || p == 0x1F801DAA)) fprintf(stderr, "[spuirq] frame %llu reg %04x <- %04x\n", (unsigned long long)x->frames, (unsigned)(p & 0xFFFF), (unsigned)(uint16_t)v);
        if (p == 0x1F801DAA && !(v & 0x40)) x->spu_irqflag = 0;                       /* SPUCNT bit 6 = 0 acknowledges the SPU IRQ */
        if (p == 0x1F801DA6) x->spu_addr = ((uint32_t)(uint16_t)v * 8) & 0x7FFFF;
        else if (p == 0x1F801DA8) { uint16_t h = (uint16_t)v; memcpy(x->spuram + (x->spu_addr & 0x7FFFE), &h, 2); x->spu_addr = (x->spu_addr + 2) & 0x7FFFF; }
        return;
    }
    if (size != 4 && (base == 0x1F8010F4 || base == 0x1F8010F0)) {                     /* byte/halfword writes to DPCR/DICR: merge into the 32-bit register */
        uint32_t sh = (p & 3) * 8, mask = (size == 1 ? 0xFFu : 0xFFFFu) << sh, old = base == 0x1F8010F4 ? (x->dicr & 0x00FFFFFFu) : x->dpcr;
        uint32_t nv = (old & ~mask) | ((v << sh) & mask);
        if (base == 0x1F8010F4 && (mask & 0xFF000000u) == 0) nv &= 0x00FFFFFFu;     /* no flag acknowledge unless the flag byte is written */
        io_write(x, base, 4, nv, ok);
        return;
    }
    if (size != 4 && !(base == 0x1F801074 || base == 0x1F801070 || (base >= 0x1F801100 && base < 0x1F801130))) { *ok = bad(x, "sub-word write to %08x (%u bytes) is unproven", p, size); return; }
    if (base >= 0x1F801000 && base <= 0x1F801020) x->memctl[(base - 0x1F801000) >> 2] = v;
    else if (base == 0x1F801810) gp0_write(x, v);
    else if (base == 0x1F801814) gp1_write(x, v);
    else if (base == 0x1F801070) {                           /* acknowledge: bits written as 0 are cleared; bits outside the written part stay */
        uint32_t keep = size == 4 ? 0 : size == 2 ? (0xFFFFu << (((p & 2) ? 0 : 16))) : ~(0xFFu << ((p & 3) * 8));
        uint32_t val = size == 4 ? v : size == 2 ? ((v & 0xFFFF) << ((p & 2) * 8)) : ((v & 0xFF) << ((p & 3) * 8));
        x->i_stat &= val | keep;
    }
    else if (base == 0x1F801074) { if (size == 1 || (size == 2 && (p & 2)) ) { *ok = bad(x, "I_MASK write at %08x (%u bytes) unproven", p, size); } else x->i_mask = v & 0xFFFF; }
    else if (base == 0x1F8010F0) {
        uint32_t old = x->dpcr;
        x->dpcr = v;
        for (int n = 0; n < 7; n++) {                         /* a channel that was started while disabled begins when it is enabled */
            if (!((old >> (4 * n + 3)) & 1) && ((v >> (4 * n + 3)) & 1) && (x->ch[n].chcr & (1u << 24)) && (n == 0 || n == 1 || n == 2 || n == 3 || n == 4 || n == 6)) {
                int trig = ((x->ch[n].chcr >> 9) & 3) == 0 ? (int)((x->ch[n].chcr >> 28) & 1) : 1;
                if (trig && !dma_start(x, n)) *ok = 0;
            }
        }
    }
    else if (base >= 0x1F801100 && base < 0x1F801130 && (base & 0xF) <= 8) { int t = (base - 0x1F801100) >> 4; unsigned r = (base & 0xF) >> 2; { char k[12]; snprintf(k, sizeof k, "TMR%u.%u", t, r); gpulog_note(k, v & 0xFFFF); }   /* PS1_GPULOG also lists timer register writes */
        if (r == 0) x->t_value[t] = v & 0xFFFF; else if (r == 1) { x->t_mode[t] = v & 0xFFFF; x->t_value[t] = 0; x->t_done[t] = 0; } else x->t_target[t] = v & 0xFFFF; }
    else if (base == 0x1F8010F4) {                           /* bits 0-23 writable, flags 24-30 acknowledged by writing 1 */
        uint32_t flags = (x->dicr >> 24) & 0x7F & ~((v >> 24) & 0x7F);
        x->dicr = (v & 0x00FF803Fu) | (flags << 24) | (x->dicr & 0x80000000u);
        uint32_t en = (x->dicr >> 16) & 0x7F;
        uint32_t master = ((x->dicr >> 15) & 1) | (((x->dicr >> 23) & 1) & ((flags & en) != 0));
        x->dicr = (x->dicr & 0x7FFFFFFFu) | (master << 31);
    } else if (base >= 0x1F801080 && base < 0x1F8010F0) {
        int n = (base - 0x1F801080) >> 4, r = (base >> 2) & 3;
        if (getenv("PS1_DMALOG")) fprintf(stderr, "[dma] write channel %d reg %d <- %08x (%u bytes at %08x)\n", n, r, v, size, p);
        if ((n != 0 && n != 1 && n != 2 && n != 3 && n != 4 && n != 6)) { *ok = bad(x, "write to DMA channel register %08x (%u bytes): only channels 0, 1, 2, 3, 4 and 6 are proven used", p, size); return; }
        if (r == 0) x->ch[n].madr = v & 0xFFFFFF;
        else if (r == 1) x->ch[n].bcr = v;
        else if (r == 2) {
            if (n == 6) v = (v & 0x51000000u) | 2u;                          /* OTC control: only bits 24, 28 and 30 are writable, bit 1 (backwards step) reads 1, the rest 0 (public ps1-tests dma/otc-test, finding 0043) */
            x->ch[n].chcr = v;
            if (n == 0) x->d0_pend = x->d0_hold = 0; else if (n == 1) x->d1_pend = 0;          /* a new CHCR value replaces (aborts) a transfer that was waiting on the decoder */
            int start = (v >> 24) & 1;
            int trig = ((v >> 9) & 3) == 0 ? (int)(((v >> 28) & 1) || n == 4) : 1;      /* manual sync needs the trigger bit too, except the SPU channel whose request line is always active: it starts on the enable bit alone (public ps1-tests spu/memory-transfer, finding 0043) */
            if (start && trig && !dma_start(x, n)) *ok = 0;
        } else { *ok = bad(x, "write to unknown DMA register %08x (%u bytes)", p, size); }
    } else { *ok = bad(x, "write of %08x to unproven I/O register %08x", v, p); (void)size; }
    if (*ok && x->err[0] && strstr(x->err, "unknown GP0")) *ok = 0;
}

void io_gpu_counters(IoCtx *x, uint64_t o[8]) {
    o[0] = x->c_words; o[1] = x->c_prims; o[2] = x->c_uploads; o[3] = x->c_fills; o[4] = x->c_dma2;
    o[5] = x->disp_start; o[6] = gpustat(x); o[7] = x->c_ot;
}

int io_display_rect(IoCtx *x, int *x0, int *y0, int *w, int *h, int *d24) {
    static const int hres[4] = {256, 320, 512, 640};
    uint32_t b = x->disp_bits;
    *x0 = (int)(x->disp_start & 0x3FF); *y0 = (int)((x->disp_start >> 10) & 0x1FF);
    *w = (b & (1u << 16)) ? 368 : hres[(b >> 17) & 3];
    *h = ((b >> 19) & 1) && ((b >> 22) & 1) ? 480 : 240;
    *d24 = (int)((b >> 21) & 1);
    return !(x->disp_off & 1);
}

int io_present_rgb(IoCtx *x, uint8_t *rgb, int max_w, int max_h, int *w, int *h) {
    int x0, y0, d24;
    int on = io_display_rect(x, &x0, &y0, w, h, &d24);
    if (*w > max_w) *w = max_w;
    if (*h > max_h) *h = max_h;
    /* vertical display range GP1(07): the picture starts at scanline y1 and ends at y2; the visible NTSC window is 16..256 (a 14-line offset appeared against an
     * independent emulator when this was ignored, finding 0031); in 480-line interlaced mode every scanline is two output rows. Horizontal range is not applied. */
    int mult = *h > 240 ? 2 : 1;
    int y1 = (int)(x->vrange & 0x3FF), y2 = (int)((x->vrange >> 10) & 0x3FF);
    int off = (y1 - 16) * mult, lines = (y2 - y1) * mult;
    if (lines <= 0 || lines > *h) lines = *h;
    for (int j = 0; j < *h; j++) for (int i = 0; i < *w; i++) {
        uint8_t *o = rgb + ((size_t)j * *w + i) * 3;
        int sj = j - off;
        if (!on || sj < 0 || sj >= lines || sj >= *h) { o[0] = o[1] = o[2] = 0; continue; }
        if (d24) {                                          /* packed 24 bit: 3 bytes per pixel starting at the display x, little endian */
            size_t byte = (size_t)(x0 * 2) + (size_t)i * 3;
            const uint8_t *row = (const uint8_t *)(x->vram + ((y0 + sj) & (VH - 1)) * VW);
            o[0] = row[byte % (VW * 2)]; o[1] = row[(byte + 1) % (VW * 2)]; o[2] = row[(byte + 2) % (VW * 2)];
        } else {
            uint16_t p = x->vram[((y0 + sj) & (VH - 1)) * VW + ((x0 + i) & (VW - 1))];
            o[0] = (uint8_t)((p & 31) << 3); o[1] = (uint8_t)(((p >> 5) & 31) << 3); o[2] = (uint8_t)(((p >> 10) & 31) << 3);
        }
    }
    return on;
}

int io_dump_ppm(IoCtx *x, const char *path, int x0, int y0, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        uint16_t p = x->vram[((y0 + j) & (VH - 1)) * VW + ((x0 + i) & (VW - 1))];
        uint8_t rgb[3] = {(uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

uint64_t io_state_hash(IoCtx *x) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t regs[] = {x->i_stat, x->i_mask, x->dpcr, x->dicr, x->dma_pend, x->d0_madr ^ x->d0_left ^ (uint32_t)x->d0_pend ^ ((uint32_t)x->d0_hold << 1), x->d1_madr ^ x->d1_left ^ (uint32_t)x->d1_pend, (uint32_t)x->sio.ctrl | ((uint32_t)x->sio.state << 16), (uint32_t)x->sio.nrx | ((uint32_t)x->sio.irq_pend << 8), x->sio.pad, x->stat_texpage, x->mask_bits, x->disp_bits, x->disp_off, x->dma_dir, x->irq1,
                       x->disp_start, x->hrange, x->vrange, x->draw_tl, x->draw_br, x->draw_ofs, x->tw, x->t_value[0], x->t_value[1], x->t_value[2], x->t_mode[0], x->t_mode[1], x->t_mode[2], x->t_target[0], x->t_target[1], x->t_target[2], (uint32_t)x->frames, (uint32_t)x->c_spu_w, x->t_pend[2], x->t_done[2]};
    const uint8_t *p = (const uint8_t *)regs;
    for (size_t i = 0; i < sizeof regs; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    for (size_t i = 0; i < sizeof x->memctl; i++) { h ^= ((const uint8_t *)x->memctl)[i]; h *= 0x100000001b3ull; }
    for (size_t i = 0; i < 0x80000; i++) { h ^= x->spuram[i]; h *= 0x100000001b3ull; }
    for (size_t i = 0; i < sizeof x->spu_r; i++) { h ^= ((const uint8_t *)x->spu_r)[i]; h *= 0x100000001b3ull; }
    h ^= x->spu.hash; h *= 0x100000001b3ull; h ^= x->spu.total; h *= 0x100000001b3ull;
    p = (const uint8_t *)x->vram;
    for (size_t i = 0; i < (size_t)VW * VH * 2; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
