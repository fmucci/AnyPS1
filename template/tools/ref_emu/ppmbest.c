/* For every file of the first list, the best identical-pixel percentage against any file of the second list (same-size P6 PPMs).
 * usage: ppmbest A1.ppm A2.ppm ... -- B1.ppm B2.ppm ...      prints `A  best%  B` per A, then min/median of the bests. Build: cc -O2 -o ppmbest ppmbest.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *load(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    int w, h; if (fscanf(f, "P6 %d %d 255", &w, &h) != 2) { fclose(f); return NULL; }
    fgetc(f); *n = (size_t)w * h * 3; unsigned char *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f); return b;
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
int main(int argc, char **argv) {
    int sep = 0; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--")) sep = i;
    if (!sep) return 2;
    int na = sep - 1, nb = argc - sep - 1; size_t n = 0;
    unsigned char **B = calloc((size_t)nb, sizeof *B); size_t *M = calloc((size_t)nb, sizeof *M);
    for (int j = 0; j < nb; j++) B[j] = load(argv[sep + 1 + j], &M[j]);
    double *best = calloc((size_t)na, sizeof *best); int cnt = 0;
    for (int i = 0; i < na; i++) {
        unsigned char *a = load(argv[1 + i], &n); if (!a) continue;
        double bp = 0; int bj = -1;
        for (int j = 0; j < nb; j++) {
            if (!B[j] || M[j] != n) continue;
            size_t same = 0; for (size_t k = 0; k < n; k += 3) same += a[k] == B[j][k] && a[k + 1] == B[j][k + 1] && a[k + 2] == B[j][k + 2];
            double p = 100.0 * (double)same / (double)(n / 3); if (p > bp) { bp = p; bj = j; }
        }
        printf("%s %.2f %s\n", argv[1 + i], bp, bj >= 0 ? argv[sep + 1 + bj] : "-");
        best[cnt++] = bp; free(a);
    }
    if (cnt) { qsort(best, (size_t)cnt, sizeof *best, cmpd); printf("min %.2f median %.2f max %.2f (%d frames)\n", best[0], best[cnt / 2], best[cnt - 1], cnt); }
    return 0;
}
