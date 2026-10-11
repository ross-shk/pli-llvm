/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for decimal_ops benchmark.
 * Mirrors decimal_ops.pli: bounded per-iteration values so the
 * accumulator stays in range (see the .pli header comment). */
#include <stdio.h>

int main(void) {
    int i;
    double rate, grand, price, qty, total, tax;

    rate = 0.0875;
    grand = 0.0;

    for (i = 1; i <= 30000000; i++) {
        price = (i % 1000) * 1.99;
        qty = (i % 100) / 10 + 1;
        total = price * qty;
        tax = total * rate;
        grand = grand + total + tax;
    }

    printf("%.2f\n", grand);
    return 0;
}
