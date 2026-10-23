/* Hardware register model shared by the runtime and the test oracle (queue P3-2/P4-2): I_STAT/I_MASK, DMA controller (channels 2 and 6 only),
 * GPU (GP0/GP1/GPUSTAT/GPUREAD, VRAM). Anything not listed in docs/findings/0022 is "unproven": the access returns ok = 0 and the caller aborts. */
#ifndef PS1_IO_H
#define PS1_IO_H
#include <stdint.h>
typedef struct IoCtx IoCtx;
IoCtx *io_new(uint8_t *ram);                       /* ram = 2 MiB guest RAM owned by the caller */
void io_free(IoCtx *x);
uint32_t io_read(IoCtx *x, uint32_t phys, unsigned size, int *ok);
void io_write(IoCtx *x, uint32_t phys, unsigned size, uint32_t v, int *ok);
int io_irq_pending(IoCtx *x);                       /* (I_STAT & I_MASK) != 0 */
void io_vblank_tick(IoCtx *x);                     /* one frame of host-owned time passes */
int io_cd_open(IoCtx *x, const char *raw_bin_path);   /* attaches the disc image (raw MODE2/2352) to the CD controller model; 1 on success */
int io_timer_irq_next(IoCtx *x);                    /* raises the next queued root-counter interrupt in I_STAT (bits 4-6); returns the counter number + 1, or 0 if none */
uint32_t io_imask(IoCtx *x);                        /* I_MASK */
void io_irq_ack(IoCtx *x, uint32_t bits);           /* clears bits of I_STAT (what the BIOS interrupt handler does for an auto-acknowledged source) */
int io_sio_irq_next(IoCtx *x);                    /* raises the next queued SIO0 ACK interrupt (I_STAT bit 7); returns 1 if one was raised */
void io_sio_set_pad(IoCtx *x, uint32_t bios_word);   /* current pad-1 buttons in the B(16h) format (active low) */
int io_dma_pending(const IoCtx *x);                /* nonzero while a DMA-completion interrupt is queued */
int io_dma_irq_next(IoCtx *x);                    /* raises the next queued DMA-completion interrupt (DICR flag, I_STAT bit 3); returns 1 if I_STAT changed */
int io_cd_step(IoCtx *x);                           /* fires at most one pending CD interrupt (extra interrupts within a frame); returns 1 if one fired */
const char *io_last_error(IoCtx *x);               /* text for the last ok = 0 */
uint16_t *io_vram(IoCtx *x);                       /* 1024 x 512 halfwords */
void io_gpu_counters(IoCtx *x, uint64_t out[8]);   /* [0]=gp0 words, [1]=primitives, [2]=vram uploads, [3]=fills, [4]=dma2 transfers, [5]=display start (y<<10|x), [6]=gpustat, [7]=ot packets */
/* display area as the GPU would scan it out: top-left VRAM position, size and depth (0 = 15 bit, 1 = 24 bit); returns 0 when the display is disabled */
int io_display_rect(IoCtx *x, int *x0, int *y0, int *w, int *h, int *depth24);
int io_present_rgb(IoCtx *x, uint8_t *rgb, int max_w, int max_h, int *w, int *h);   /* converts the display area to packed RGB24; 0 if disabled */
int io_spu_pull(IoCtx *x, int16_t *out, int max_frames);   /* drains generated stereo frames (L,R interleaved, 44100 Hz); returns the count */
void io_spu_capture(IoCtx *x, const char *path);    /* host tools: append every generated frame to a raw s16le stereo 44100 Hz file */
void io_spu_capture_close(IoCtx *x);
uint64_t io_spu_total(IoCtx *x);                    /* frames generated so far */
uint64_t io_spu_hash(IoCtx *x);                     /* hash of every generated sample */
void io_gpulog_phase(const char *name);              /* debug aid: names the current executable for the PS1_GPULOG output */
int io_dump_ppm(IoCtx *x, const char *path, int x0, int y0, int w, int h);   /* 15-bit VRAM rectangle as P6 PPM */
uint64_t io_state_hash(IoCtx *x);                  /* hash of every register and VRAM, for differential tests */
#endif
