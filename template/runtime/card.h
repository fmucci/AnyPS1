/* Virtual memory card and the BIOS memory-card / file calls on top of it (queue P3-3). One implementation shared by the runtime and the oracle.
 * Format: psx-spx "Memory Card Data Format" (128 KiB = 16 blocks of 8 KiB, block 0 = header + directory). Calls: psx-spx "BIOS Memory Card
 * Functions" and "BIOS File Functions". Only slot 0 ("bu00:") exists; the hot-swap "card changed" flag is not modelled. */
#ifndef PS1_CARD_H
#define PS1_CARD_H
#include <stdint.h>
typedef struct Card Card;
Card *card_new(const char *backing_path);   /* NULL = no card inserted; existing file is loaded, a missing file is created formatted on first write */
void card_free(Card *c);
void card_set_path(Card *c, const char *backing_path);   /* later writes go to another file (forked branches of a run must not share one card file) */
int card_event_delay(Card *c);               /* frames the events returned by the last card_call must wait before delivery */
unsigned card_gen(Card *c);                  /* changes whenever the image does */
int card_save(Card *c);                      /* writes the image to the backing file if it changed; 1 on success */
const uint8_t *card_image(Card *c);          /* 131072 bytes */
/* Runs BIOS call table/number `t`/`n` with arguments a0..a3 against guest RAM `ram` (2 MiB, masked addresses). Returns 1 if the call belongs to the
 * card layer (result in *ret; events to deliver, in order, in ev[k][0]=class, ev[k][1]=spec, count in *nev (max 8)), 0 otherwise. */
int card_call(Card *c, uint8_t *ram, char t, unsigned n, const uint32_t a[4], uint32_t *ret, uint32_t ev[8][2], int *nev);
#endif
