/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM */
/* C baseline for bcd_decimal benchmark */
/* Match = ~ by design: C double has ~15-16 significant digits; */
/* PL/I DECIMAL(25,4) is exact to 25 digits. */
#include <stdio.h>

int main(void) {
    int i, n;
    double price, qty, total, tax, grand, rate;

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
