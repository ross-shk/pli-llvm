/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for condition_ops benchmark */
#include <stdio.h>

int main(void) {
    int i, n, sum, x, y;
    int a[100];

    n = 100;
    sum = 0;
    for (i = 0; i < n; i++) {
        a[i] = (i + 1) * (i + 1);
        sum = sum + a[i];
    }

    x = 1000;
    y = 1000;
    for (i = 0; i < 50; i++) {
        x = x + y;
        y = y + 1;
    }

    printf("%d %d %d\n", sum, x, y);
    return 0;
}
