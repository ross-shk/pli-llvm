/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for math_functions benchmark */
#include <stdio.h>
#include <math.h>

int main(void) {
    int i, j, n;
    double x, y, sum;

    n = 5000;
    sum = 0.0;
    for (i = 1; i <= n; i++) {
        x = i / 100.0;           /* matches plic's exact DECIMAL(i,2)->FLOAT = (double)stored/10^scale */
        for (j = 1; j <= 1000; j++) {
            y = sin(x) + cos(x) + sqrt(x + 1.0) + exp(x) + log(x + 1.0);
            sum = sum + y;
            x = x + 0.001;
        }
    }

    printf("%.15g\n", sum);
    return 0;
}
