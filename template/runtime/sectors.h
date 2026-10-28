/* Host-side sector source: a MODE2/2352 .bin image, or a directory made by tools/disc_unpack.py (loose files + disc.map). */
#ifndef PS1_SECTORS_H
#define PS1_SECTORS_H
#include <stdint.h>
typedef struct Sectors Sectors;
Sectors *sec_open(const char *path);                 /* NULL if the path is neither a readable file nor a directory with a usable disc.map */
void sec_close(Sectors *s);
void sec_reopen(Sectors *s);                         /* drop the open file handles (they are reopened on demand) */
int sec_read(Sectors *s, uint32_t lba, uint8_t out[2352]);   /* 1 on success; cooked sectors get sync, MSF header, subheader and zero EDC/ECC rebuilt */
#endif
