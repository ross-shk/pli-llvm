/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for matrix_float benchmark */
#include <stdio.h>
#include <math.h>

int main(void) {
    int n = 400;
    int i, j, k;
    long chk;
    double sum, x;
    double a[400][400], b[400][400], c[400][400];

    for (i = 1; i <= n; i++) {
        for (j = 1; j <= n; j++) {
            x = (i + j) * 0.01;
            a[i-1][j-1] = sin(x) + cos(x * 0.5);
            b[i-1][j-1] = (i - j) * 0.01;
            b[i-1][j-1] = sqrt(b[i-1][j-1] * b[i-1][j-1] + 1.0);
            c[i-1][j-1] = 0.0;
        }
    }

    for (i = 1; i <= n; i++) {
        for (j = 1; j <= n; j++) {
            sum = 0.0;
            for (k = 1; k <= n; k++) {
                sum += a[i-1][k-1] * b[k-1][j-1];
            }
            c[i-1][j-1] = sum;
        }
    }

    chk = 0;
    for (i = 1; i <= n; i++) {
        for (j = 1; j <= n; j++) {
            chk += (long)floor(c[i-1][j-1]);
            chk += (long)floor(c[i-1][j-1] * 1000.0);
        }
    }
    printf("%ld\n", chk);
    return 0;
}
