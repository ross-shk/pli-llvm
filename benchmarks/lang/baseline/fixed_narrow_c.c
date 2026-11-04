/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for fixed_narrow: the same workload in int16_t, the C type
 * with the declared precision of FIXED BINARY(15) plus its sign. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int16_t a[1000];
    int64_t s = 0;

    for (int i = 0; i < 1000; i++)
        a[i] = (int16_t)((i + 1) * 7 - 3); /* PL/I indexes from 1 */
    for (int i = 1; i <= 100000000; i++) {
        int j = i % 1000;
        int16_t x = a[j];
        int16_t y = (int16_t)(x * 3 + 1);
        s += y;
    }
    printf("%lld\n", (long long)s);
    return 0;
}
