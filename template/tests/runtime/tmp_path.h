/* Portable temporary file names for the C tests (no mkstemp/unistd: Windows has neither). Fills `out` with a fresh path in $TMPDIR, $TEMP, $TMP or /tmp. */
#ifndef PS1_TMP_PATH_H
#define PS1_TMP_PATH_H
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void tmp_path(char *out, size_t n, const char *tag) {
    static unsigned counter;
    const char *d = getenv("TMPDIR"); if (!d) d = getenv("TEMP"); if (!d) d = getenv("TMP"); if (!d) d = "/tmp";
    snprintf(out, n, "%s/ps1_%s_%lx_%u.tmp", d, tag, (unsigned long)time(NULL) ^ ((unsigned long)clock() << 8), counter++);
}
#endif
