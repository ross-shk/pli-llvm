/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for loop_fusion: three separate producer loops (unfused) over
 * the same arrays, plus a shared reduction loop identical to the PL/I
 * version. 0-based indexing with i + 1 to match PL/I's 1-based subscripts. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int32_t a[100000];
    int32_t b[100000];
    int32_t c[100000];
    int32_t i, k;
    int64_t s = 0;

    for (k = 1; k <= 100; k++) {
        for (i = 0; i < 100000; i++)
            a[i] = (int32_t)(i + 1);       /* PL/I indexes from 1 */
        for (i = 0; i < 100000; i++)
            b[i] = a[i] * 2;
        for (i = 0; i < 100000; i++)
            c[i] = b[i] + 1;
    }
    for (i = 0; i < 100000; i++)
        s += c[i];
    printf("%lld\n", (long long)s);
    return 0;
}
