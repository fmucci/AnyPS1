/* Host-side CD image access (queue P2-3): ISO 9660 over a raw MODE2/2352 .bin, user data = 2048 bytes at offset 24 of each frame. */
#ifndef PS1_DISC_H
#define PS1_DISC_H
#include <stdint.h>
typedef struct RtDisc RtDisc;
void rt_disc_reopen(RtDisc *d);                      /* reopen with its own file offset (after fork()) */
RtDisc *rt_disc_open(const char *bin_path);          /* NULL if the file cannot be opened or has no ISO 9660 descriptor */
void rt_disc_close(RtDisc *d);
/* path like "\\INTRO.EXE;1" (case-insensitive, version optional); returns 1 and fills lba/size/name (NUL-terminated, max 31 chars) or 0 */
int rt_disc_find(RtDisc *d, const char *path, uint32_t *lba, uint32_t *size, char name[32]);
int rt_disc_read(RtDisc *d, uint32_t lba, uint32_t nsectors, uint8_t *dst);   /* 1 on success */
#endif
