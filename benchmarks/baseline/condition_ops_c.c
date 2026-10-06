/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for condition_ops benchmark */
#include <stdio.h>

int main(void) {
    int i, n, x, y, bad, rep;
    long long sum;
    long long a[250000];

    n = 250000;
    sum = 0;
    for (rep = 0; rep < 300; rep++) {
        for (i = 0; i < n; i++) {
         a[i] = (long long)(i + 1) * (i + 1) + ((i + 1) % 3);
        sum = sum + a[i];
        }
    }

    x = 1000;
    y = 1000;
    for (i = 0; i < 50; i++) {
        x = x + y;
        y = y + 1;
    }

    /* Region 2: handler-cost analogue — every 16th iteration counts as
       a "fired" handler (equivalent to PL/I's SUBSCRIPTRANGE resume). */
    bad = 0;
    for (i = 1; i <= 1600; i++) {
        if (i % 16 == 0) bad++;
        else a[(i % 100)] = i;
    }

    printf("%lld %d %d %d\n", sum, x, y, bad);
    return 0;
}
