#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sectors.h"

#define FRAME 2352
typedef struct { char kind; uint32_t lba, n, fsec; uint8_t sh[8]; int file; } Ext;
struct Sectors { Ext *e; int ne; char (*names)[1100]; FILE **fp; int nf; };

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int file_index(Sectors *s, const char *name) {
    for (int i = 0; i < s->nf; i++) if (!strcmp(s->names[i], name)) return i;
    s->names = realloc(s->names, sizeof *s->names * (s->nf + 1));
    s->fp = realloc(s->fp, sizeof *s->fp * (s->nf + 1));
    if (!s->names || !s->fp) return -1;
    snprintf(s->names[s->nf], sizeof *s->names, "%s", name);
    s->fp[s->nf] = NULL;
    return s->nf++;
}
static int add(Sectors *s, Ext x) { Ext *p = realloc(s->e, sizeof *p * (s->ne + 1)); if (!p) return 0; s->e = p; s->e[s->ne++] = x; return 1; }
static int cmp_ext(const void *a, const void *b) { uint32_t x = ((const Ext *)a)->lba, y = ((const Ext *)b)->lba; return x < y ? -1 : x > y; }

static int load_map(Sectors *s, const char *dir) {
    char mp[1100]; snprintf(mp, sizeof mp, "%s/disc.map", dir);
    FILE *f = fopen(mp, "r");
    if (!f) return 0;
    char line[1300];
    while (fgets(line, sizeof line, f)) {
        char kind; unsigned lba, n, fsec = 0; char rel[1000], sh[32] = "";
        if (line[0] == '#') continue;
        int k = sscanf(line, "%c %u %u %999s %31s %u", &kind, &lba, &n, rel, sh, &fsec);
        if ((kind != 'c' && kind != 'r') || k < 4 || (kind == 'c' && k < 6) || strlen(sh) != (kind == 'c' ? 16u : 0u)) { fclose(f); return 0; }
        Ext x = {0}; x.kind = kind; x.lba = lba; x.n = n; x.fsec = fsec;
        for (int i = 0; i < 8 && kind == 'c'; i++) { unsigned v; if (sscanf(sh + 2 * i, "%2x", &v) != 1) { fclose(f); return 0; } x.sh[i] = (uint8_t)v; }
        char full[1100]; snprintf(full, sizeof full, "%s/%s", dir, rel);
        if ((x.file = file_index(s, full)) < 0 || !add(s, x)) { fclose(f); return 0; }
    }
    fclose(f);
    qsort(s->e, s->ne, sizeof *s->e, cmp_ext);
    return s->ne > 0;
}

Sectors *sec_open(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    Sectors *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (S_ISDIR(st.st_mode)) { if (!load_map(s, path)) { sec_close(s); return NULL; } }
    else {
        Ext x = {0}; x.kind = 'r'; x.lba = 0; x.n = (uint32_t)(st.st_size / FRAME);
        if ((x.file = file_index(s, path)) < 0 || !add(s, x)) { sec_close(s); return NULL; }
    }
    for (int i = 0; i < s->nf; i++) { s->fp[i] = fopen(s->names[i], "rb"); if (!s->fp[i]) { sec_close(s); return NULL; } }   /* fail early on a missing file */
    return s;
}
void sec_reopen(Sectors *s) { if (s) for (int i = 0; i < s->nf; i++) if (s->fp[i]) { fclose(s->fp[i]); s->fp[i] = NULL; } }
void sec_close(Sectors *s) { if (!s) return; sec_reopen(s); free(s->e); free(s->names); free(s->fp); free(s); }

int sec_read(Sectors *s, uint32_t lba, uint8_t out[FRAME]) {
    if (s->ne == 0) return 0;
    int lo = 0, hi = s->ne - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (s->e[m].lba <= lba) lo = m; else hi = m - 1; }
    const Ext *x = &s->e[lo];
    if (lba < x->lba || lba >= x->lba + x->n) return 0;
    FILE **fp = &s->fp[x->file];
    if (!*fp && !(*fp = fopen(s->names[x->file], "rb"))) return 0;
    uint32_t k = lba - x->lba;
    if (x->kind == 'r') return fseek(*fp, (long)((uint64_t)k * FRAME), SEEK_SET) == 0 && fread(out, 1, FRAME, *fp) == FRAME;
    memset(out, 0, FRAME);
    memset(out + 1, 0xFF, 10);
    int a = (int)lba + 150;
    out[12] = (uint8_t)bcd(a / 4500); out[13] = (uint8_t)bcd((a / 75) % 60); out[14] = (uint8_t)bcd(a % 75); out[15] = 2;
    memcpy(out + 16, x->sh, 8);
    if (fseek(*fp, (long)((uint64_t)(x->fsec + k) * 2048), SEEK_SET) != 0) return 0;
    size_t got = fread(out + 24, 1, 2048, *fp);                    /* the last sector of a file is short: the rest stays zero */
    return got > 0;
}
