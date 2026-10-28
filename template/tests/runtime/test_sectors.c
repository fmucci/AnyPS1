/* usage: test_sectors A B N   - the first N sectors read from A and B must match (sync, header, subheader, 2048 data bytes; EDC/ECC are not compared) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sectors.h"
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    Sectors *a = sec_open(argv[1]), *b = sec_open(argv[2]);
    if (!a || !b) { puts("test_sectors: cannot open a source"); return 1; }
    if (sec_open("/nonexistent/disc") != NULL) { puts("test_sectors: opened a missing path"); return 1; }
    uint8_t x[2352], y[2352]; unsigned bad = 0, n = (unsigned)atoi(argv[3]);
    for (unsigned l = 0; l < n; l++) {
        int ra = sec_read(a, l, x), rb = sec_read(b, l, y);
        if (ra != rb || (ra && memcmp(x, y, 24 + 2048))) { if (bad++ < 5) printf("lba %u differs (%d/%d)\n", l, ra, rb); }
    }
    if (sec_read(b, n + 1000, y)) { puts("test_sectors: read past the end"); return 1; }
    sec_reopen(b); if (!sec_read(b, 0, y)) { puts("test_sectors: read after reopen failed"); return 1; }
    printf("test_sectors: %u sectors, %u differ\n", n, bad);
    sec_close(a); sec_close(b);
    return bad != 0;
}
