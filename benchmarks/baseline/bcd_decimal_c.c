/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for bcd_decimal benchmark */
/* Match = ~ by design: PL/I exact BCD decimal vs C double (see DECIMAL-PERF-PLAN.md §8). */
#include <stdio.h>

int main(void) {
    int i, n;
    double rate, grand, price, qty, total, tax;

    rate = 0.0875;
    grand = 0.0;
    n = 20000000;

    for (i = 1; i <= n; i++) {
        price = i * 1.9999;
        qty = i / 10 + 1;
        total = price * qty;
        tax = total * rate;
        grand = grand + total + tax;
    }

    printf("%.4f\n", grand);
    return 0;
}
