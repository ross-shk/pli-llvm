/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for math_functions benchmark */
#include <stdio.h>
#include <math.h>

int main(void) {
    int i, j, n;
    double x, y, sum;

    n = 200;
    sum = 0.0;
    for (i = 1; i <= n; i++) {
        x = i * 0.01;
        for (j = 1; j <= 1000; j++) {
            y = sin(x) + cos(x) + sqrt(x + 1.0) + exp(x) + log(x + 1.0);
            sum = sum + y;
            x = x + 0.001;
        }
    }

    printf("%.6g\n", sum);
    return 0;
}
