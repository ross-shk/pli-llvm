/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for array_param: the same loop, summing a fixed-size array
 * parameter. Clang knows the array's size from the call, so it vectorizes
 * the loop; the PL/I version has to be told the shape of its parameter
 * (ADR-271). */
#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) static int64_t total(int32_t *x) {
    int64_t t = 0;
    for (int32_t i = 0; i < 1000; i++)
        t += x[i];
    return t;
}

int main(void) {
    static int32_t a[1000];

    /* PL/I's a(1) is C's a[0]: the PL/I loop counts from one. */
    for (int32_t i = 1; i <= 1000; i++)
        a[i - 1] = i * 7 - 3;

    int64_t s = 0;
    for (int32_t i = 1; i <= 1000000; i++)
        s += total(a);

    printf("%lld\n", (long long)s);
    return 0;
}
