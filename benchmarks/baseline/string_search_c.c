/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for string_search benchmark */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    int i, n, pos;
    long long sum;
    char *buffer;
    const char *base = "abcdefghijklmnopqrstuvwxyz0123456789 ";
    const char *needle = "SENTINEL";

    /* Build ~76KB buffer: 2048 copies of 37-char base */
    buffer = malloc(75776 + 13 + 1);  /* base + sentinel + NUL */
    if (!buffer) return 1;
    buffer[0] = '\0';
    for (i = 0; i < 2048; i++) {
        strcat(buffer, base);
    }
    /* Append sentinel tail holding both needles (same as PL/I) */
    strcat(buffer, "ZZSENTINELZZ");

    sum = 0;
    n = 48000;
    for (i = 0; i < n; i++) {
        /* Poke mirrors PL/I (1-based MOD(i,70000)+1); never the tail */
        buffer[(i + 1) % 70000] = 'x';
        char *found = strstr(buffer, needle);
        if (found) {
            pos = (int)(found - buffer) + 1;  /* 1-based position */
            sum += pos;
        }
    }

    printf("%lld\n", sum);
    free(buffer);
    return 0;
}
