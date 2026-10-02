/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for scalar_arith benchmark */
#include <stdio.h>

int main(void) {
    int i, sum;
    double x, y, z;

    sum = 0;
    for (i = 1; i <= 5000; i++) {
        sum = sum + i;
    }

    x = 0.0;
    y = 1.0;
    for (i = 1; i <= 500; i++) {
        z = x + y * 2.5;
        x = y;
        y = z;
    }

    printf("%d %.6g %.6g\n", sum, x, y);
    return 0;
}
