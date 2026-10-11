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
    int base_len = strlen(base);

    /* Build ~76KB buffer: 2048 copies of 37-char base */
    buffer = malloc(75776 + 13 + 1);  /* base + sentinel + NUL */
    if (!buffer) return 1;
    buffer[0] = '\0';
    for (i = 0; i < 2048; i++) {
        strcat(buffer, base);
    }
    /* Append sentinel at end (same as PL/I || 'ZZSENTINELZZ') */
    strcat(buffer, "ZZSENTINELZZ");

    sum = 0;
    n = 3000;
    for (i = 0; i < n; i++) {
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
