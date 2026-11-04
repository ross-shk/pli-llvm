/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for index_flip: INDEX with a constant needle uses strstr
   with a constant pattern; INDEX with a variable needle uses strstr with
   a variable pattern. The PL/I per-call-site predicate chooses table vs
   memchr for the two call sites. */
#include <stdio.h>
#include <string.h>

int main(void) {
    long long i, n, pos, sum;
    char buffer[100001];
    char needle[9];
    const char *const_needle = "SENTINEL";

    buffer[0] = '\0';
    for (i = 0; i < 2000; i++) {
        strcat(buffer, "abcdefghijklmnopqrstuvwxyz0123456789 ");
    }
    strcat(buffer, "ZZSENTINELZZ");

    sum = 0;
    n = 50000;

    /* Constant needle -> table path */
    for (i = 0; i < n; i++) {
        char *found = strstr(buffer, const_needle);
        pos = found ? (long long)(found - buffer) + 1 : 0;
        sum += pos;
    }

    /* Variable needle -> llvm path */
    strcpy(needle, "SENTINEL");
    for (i = 0; i < n; i++) {
        char *found = strstr(buffer, needle);
        pos = found ? (long long)(found - buffer) + 1 : 0;
        sum += pos;
    }

    printf("%lld\n", sum);
    return 0;
}
