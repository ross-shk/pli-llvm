/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM */
/* C baseline for file_io benchmark */
/* Match = exact: both sides produce the same checksum integer. */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int i, n, val, checksum;
    FILE *f;
    /* Relative scratch file: CWD-writable on Windows and Unix alike. */
    const char *path = "pli_fileio_test.tmp";

    n = 500000;

    /* WRITE phase */
    f = fopen(path, "w");
    if (!f) { printf("ERROR: cannot open output\n"); return 1; }
    for (i = 1; i <= n; i++) {
        val = (i * 7) % 1000;
        fprintf(f, "%d\n", val);
    }
    fclose(f);

    /* READ phase */
    checksum = 0;
    f = fopen(path, "r");
    if (!f) { printf("ERROR: cannot open input\n"); return 1; }
    for (i = 1; i <= n; i++) {
        if (fscanf(f, "%d", &val) != 1) {
            fclose(f);
            printf("ERROR: read failed at %d\n", i);
            remove(path);
            return 1;
        }
        checksum += val;
    }
    fclose(f);

    /* Temp file cleanup (P2 checklist item 3) */
    remove(path);

    printf("%d\n", checksum);
    return 0;
}
