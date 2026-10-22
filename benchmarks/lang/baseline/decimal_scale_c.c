/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 * C baseline for decimal_scale: exact fixed point at the same scales, so
 * the two programs must agree to the last digit. The tax rate has scale 4
 * and the price scale 2, so the product carries scale 6 and the assignment
 * back to scale 2 is a rescale with round-half-away rounding. */
#include <stdio.h>
#include <stdint.h>

/* Rescale a scale-6 value to scale 2: divide by 10^4, rounding half away
   from zero, the way PL/I rounds a narrowed decimal. */
static int64_t rescale6to2(int64_t x) {
    int64_t q = x / 10000;
    int64_t r = x % 10000;
    if (r >= 5000)
        q += 1;
    else if (r <= -5000)
        q -= 1;
    return q;
}

int main(void) {
    const int64_t rate = 875;             /* 0.0875 at scale 4 */
    int64_t grand = 0;                    /* scale 2 */
    for (int i = 1; i <= 60000000; i++) {
        int64_t price = (i % 1000) * 199; /* scale 2 */
        int64_t qty = (i % 100) / 10 + 1;  /* scale 0 */
        int64_t total = price * qty;      /* scale 2 */
        int64_t tax = rescale6to2(total * rate);
        grand += total + tax;
    }
    printf("%lld.%02lld\n", (long long)(grand / 100), (long long)(grand % 100));
    return 0;
}
