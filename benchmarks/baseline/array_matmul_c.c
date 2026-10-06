/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for array_matmul benchmark */
#include <stdio.h>

int main(void) {
    int i, j, k, n;
    long long sum;
    int a[400][400], b[400][400], c[400][400];

    n = 400;

    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            a[i][j] = (i + 1) + (j + 1);
            b[i][j] = (i + 1) - (j + 1);
            c[i][j] = 0;
        }
    }

    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            sum = 0;
            for (k = 0; k < n; k++) {
                sum = sum + a[i][k] * b[k][j];
            }
            c[i][j] = sum;
        }
    }

    sum = 0;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            sum = sum + c[i][j];
        }
    }

    printf("%lld\n", sum);
    return 0;
}
