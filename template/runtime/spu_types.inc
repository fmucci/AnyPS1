/* SPU state types (included by io.c before IoCtx) */
#include <stdint.h>
#define SPU_RING (1 << 16)
#define XA_RING (1 << 16)
typedef struct {
    int on, phase;                 /* phase: 0 attack, 1 decay, 2 sustain, 3 release */
    int level, adsr_cnt;           /* ADSR level 0..7FFF and the wait counter (44.1 kHz cycles) */
    uint32_t cur;                  /* current byte address of the ADPCM block */
    uint32_t rep;                  /* repeat address (bytes) */
    uint32_t counter;              /* pitch counter: bit 12.. = sample index within the block */
    int s[28 + 3];                 /* [0..2] = the 3 previous samples, then the 28 of the current block */
    int old, older;
    int out;                       /* last output (before volume) for pitch modulation */
    int blk_flags;
} Voice;
typedef struct {
    Voice v[24];
    uint32_t endx;
    uint32_t noise_timer; int noise_level;
    uint32_t rev_addr;             /* reverb buffer address (bytes) */
    int rev_l, rev_r, rev_phase;
    int16_t ring[SPU_RING * 2];
    uint32_t rhead, rtail;         /* frames written / read */
    uint64_t total, dropped, hash;
    FILE *cap;                     /* optional raw s16le stereo capture (host tool, not part of the hashed state) */
    /* XA-ADPCM input (decoded 37800/18900 Hz samples at 44100 Hz are produced on demand) */
    int16_t xa[2][XA_RING]; uint32_t xa_w, xa_r; uint32_t xa_frac; int xa_rate_div;
    int xa_old[2], xa_older[2]; int xa_prev[2];
} Spu;

