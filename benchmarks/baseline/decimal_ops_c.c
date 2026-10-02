/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for decimal_ops benchmark */
#include <stdio.h>

int main(void) {
    int i;
    double rate, grand, price, qty, total, tax;

    rate = 0.0875;
    grand = 0.0;

    for (i = 1; i <= 5000; i++) {
        price = i * 1.99;
        qty = i / 10 + 1;
        total = price * qty;
        tax = total * rate;
        grand = grand + total + tax;
    }

    printf("%.2f\n", grand);
    return 0;
}
