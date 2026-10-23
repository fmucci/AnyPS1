#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disc.h"

struct RtDisc { FILE *f; char path[1024]; uint32_t root_lba, root_size; };
#define FRAME 2352
#define HDR 24

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int rt_disc_read(RtDisc *d, uint32_t lba, uint32_t n, uint8_t *dst) {
    uint8_t raw[FRAME];
    for (uint32_t k = 0; k < n; k++) {
        if (fseek(d->f, (long)(lba + k) * FRAME, SEEK_SET) != 0 || fread(raw, 1, FRAME, d->f) != FRAME) return 0;
        memcpy(dst + (size_t)k * 2048, raw + HDR, 2048);
    }
    return 1;
}

RtDisc *rt_disc_open(const char *path) {
    RtDisc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->f = fopen(path, "rb");
    uint8_t pvd[2048];
    if (!d->f || !rt_disc_read(d, 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) { rt_disc_close(d); return NULL; }
    d->root_lba = le32(pvd + 156 + 2);
    d->root_size = le32(pvd + 156 + 10);
    return d;
}
void rt_disc_reopen(RtDisc *d) { if (d && d->f) { fclose(d->f); d->f = fopen(d->path, "rb"); } }
void rt_disc_close(RtDisc *d) { if (d) { if (d->f) fclose(d->f); free(d); } }

static int upper_eq(const char *a, size_t na, const char *b, size_t nb) {
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++) if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return 0;
    return 1;
}

/* look `part` up in directory (lba,size); matches the stored name exactly (case-insensitive) or, when `part` has no ';', the stored name without its version */
static int lookup(RtDisc *d, uint32_t lba, uint32_t size, const char *part, size_t plen, uint32_t *elba, uint32_t *esize, int *isdir, char name[32]) {
    uint8_t *buf = malloc((size_t)((size + 2047) / 2048) * 2048);
    if (!buf) return 0;
    int found = 0;
    if (!rt_disc_read(d, lba, (size + 2047) / 2048, buf)) { free(buf); return 0; }
    int has_ver = memchr(part, ';', plen) != NULL;
    for (uint32_t off = 0; off < size && !found;) {
        uint8_t ln = buf[off];
        if (ln == 0) { off = (off / 2048 + 1) * 2048; continue; }
        const uint8_t *rec = buf + off;
        off += ln;
        size_t nl = rec[32];
        const char *nm = (const char *)rec + 33;
        if (nl == 1 && (nm[0] == 0 || nm[0] == 1)) continue;
        size_t cmp = nl;
        if (!has_ver) { const char *s = memchr(nm, ';', nl); if (s) cmp = (size_t)(s - nm); }
        if (upper_eq(nm, cmp, part, plen)) {
            *elba = le32(rec + 2); *esize = le32(rec + 10); *isdir = (rec[25] & 2) != 0;
            size_t k = nl < 31 ? nl : 31; memcpy(name, nm, k); name[k] = 0;
            found = 1;
        }
    }
    free(buf);
    return found;
}

int rt_disc_find(RtDisc *d, const char *path, uint32_t *lba, uint32_t *size, char name[32]) {
    uint32_t cl = d->root_lba, cs = d->root_size;
    const char *p = path;
    for (;;) {
        while (*p == '\\') p++;
        if (!*p) return 0;
        const char *e = p;
        while (*e && *e != '\\') e++;
        uint32_t el, es; int isdir;
        if (!lookup(d, cl, cs, p, (size_t)(e - p), &el, &es, &isdir, name)) return 0;
        if (*e == 0) { if (isdir) return 0; *lba = el; *size = es; return 1; }
        if (!isdir) return 0;
        cl = el; cs = es; p = e;
    }
}
