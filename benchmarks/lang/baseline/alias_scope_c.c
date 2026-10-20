/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for alias_scope: the same copy loop, with the three arrays
 * declared disjoint so the compiler may vectorize it. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    static int32_t a[10000], b[10000], c[10000];
    int32_t *restrict ra = a;
    int32_t *restrict rb = b;
    int32_t *restrict rc = c;
    long long s = 0;

    for (int i = 0; i < 10000; i++) {
        rb[i] = i + 1;
        rc[i] = (i + 1) * 2;
    }
    for (int k = 1; k <= 60000; k++) {
        for (int i = 0; i < 10000; i++)
            ra[i] = rb[i] + rc[i] + k;
        s += ra[k % 10000];   /* PL/I reads a(mod(k,10000)+1) */
    }
    printf("%lld\n", (long long)s);
    return 0;
}
