/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for scalar_arith benchmark */
#include <stdio.h>

int main(void) {
    long long i, n, sum, hash;
    double x, y, z;

    sum = 0;
    hash = 1;
    n = 35000000;
    for (i = 1; i <= n; i++) {
        sum = sum + (hash % 100);
        hash = (hash * 1103515245LL + 12345) & 0x7FFFFFFFLL;
    }

    x = 0.0;
    y = 1.0;
    for (i = 1; i <= 500; i++) {
        z = x + y * 2.5;
        x = y;
        y = z;
    }

    printf("%lld %.15g %.15g\n", sum, x, y);
    return 0;
}
