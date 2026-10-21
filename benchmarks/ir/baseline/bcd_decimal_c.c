/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* Exact-decimal baseline for bcd_decimal (WS-E3, ADR-248).
 *
 * The PL/I program computes with FIXED DECIMAL(25,4): every value is an exact
 * decimal scaled by 10^4, and each assignment rounds to that scale. The
 * previous baseline used `double`, which cannot represent the values exactly,
 * so the two programs were not computing the same function and the ratio
 * measured the difference between two numeric models rather than the
 * compiler. This baseline computes the same function exactly, with __int128
 * scaled arithmetic.
 *
 * The PL/I statements and what they mean here (a raw value is scaled by 10^4):
 *   price = i * 1.9999;      FIXED BINARY(31) * FIXED DECIMAL(4): the raw
 *                             scaled integers multiply directly, so the
 *                             product's scale is 0+4 = 4 and the raw is
 *                             i * 19999.
 *   qty = i / 10 + 1;        an integer (both operands scale 0, so the
 *                             division truncates); the assignment to a
 *                             scale-4 target rescales it by 10^4.
 *   total = price * qty;     scale 4 * scale 4 = scale 8; the assignment
 *                             rounds that back to scale 4.
 *   tax = total * rate;      the same, with rate = 0.0875 (raw 875).
 *   grand = grand + total + tax;
 */
#include <stdio.h>

typedef __int128 i128;

/* Divide by 10^4, rounding half away from zero — PL/I's assignment
   conversion for a scaled decimal (ADR-006). */
static i128 rescale4(i128 v) {
    i128 q = v / 10000;
    i128 r = v % 10000;
    if (r == 0)
        return q;
    if (r > 0) {
        if (r >= 5000)
            q += 1;
    } else {
        if (-r >= 5000)
            q -= 1;
    }
    return q;
}

static void print_i128(i128 v) {
    char buf[48];
    int i = 0;
    int neg = v < 0;
    unsigned __int128 u = neg ? (unsigned __int128)(-v) : (unsigned __int128)v;
    do {
        buf[i++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u);
    if (neg)
        buf[i++] = '-';
    while (i--)
        putchar(buf[i]);
}

/* A scale-4 value, printed the way the PL/I program prints it. */
static void print_scaled(i128 raw) {
    i128 ip = raw / 10000;
    i128 fp = raw % 10000;
    if (fp < 0) {
        fp = -fp;
        ip -= 1;
    }
    print_i128(ip);
    putchar('.');
    printf("%04d\n", (int)fp);
}

int main(void) {
    const int n = 20000000;
    const i128 rate = 875; /* 0.0875 at scale 4 */
    i128 grand = 0;

    for (int i = 1; i <= n; i++) {
        i128 price = (i128)i * 19999;              /* i * 1.9999 */
        i128 qty = (i128)(i / 10 + 1) * 10000;     /* (i / 10 + 1) at scale 4 */
        i128 total = rescale4(price * qty);       /* the product's scale 8 -> 4 */
        i128 tax = rescale4(total * rate);         /* the same */
        grand = grand + total + tax;
    }

    print_scaled(grand);
    return 0;
}
