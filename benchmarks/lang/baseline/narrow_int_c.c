/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for narrow_int: the same workload in int16_t for narrow items,
 * int32_t for the accumulator (matching FIXED BINARY(31) = i32). */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int16_t a[1000];
    int16_t x, y;
    int32_t s = 0;

    for (int i = 0; i < 1000; i++)
        a[i] = (int16_t)((i + 1) * 7 - 3); /* PL/I indexes from 1 */
    for (int32_t i = 1; i <= 50000000; i++) {
        x = a[(int)(i % 1000)];
        y = (int16_t)(x * 3 + 1);
        s += y;
    }
    printf("%d\n", s);
    return 0;
}
