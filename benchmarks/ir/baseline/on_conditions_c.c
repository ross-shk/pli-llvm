/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for on_conditions, modelling the same dynamic condition
 * dispatch as the PL/I twin. The PL/I program establishes ON ZERODIVIDE
 * and resumes the division with 0; the C twin therefore keeps an ON-unit
 * stack, looks up the handler for the condition key when the divisor is
 * zero, and calls it, instead of hard-coding the fallback value. */
#include <stdio.h>

#define COND_ZERODIVIDE 1

typedef struct {
    int key;
    int (*fn)(void);
} on_unit;

static on_unit handler_stack[8];
static int handler_depth;

/* Look up the innermost handler for the key and run it. A bare RETURN in
 * the PL/I ON-unit resumes with 0, which is what on_zerodivide returns. */
static int raise_condition(int key) {
    for (int i = handler_depth - 1; i >= 0; --i)
        if (handler_stack[i].key == key)
            return handler_stack[i].fn();
    return 0;
}

static int on_zerodivide(void) {
    return 0;
}

static int checked_div(int a, int b) {
    if (b == 0)
        return raise_condition(COND_ZERODIVIDE);
    return a / b;
}

int main(void) {
    long long s = 0;

    handler_stack[handler_depth].key = COND_ZERODIVIDE;
    handler_stack[handler_depth].fn = on_zerodivide;
    handler_depth++;

    for (int i = 1; i <= 60000000; i++) {
        int q = checked_div(1000000, i % 100);
        s += i;
        s += q % 10;
    }

    printf("%lld\n", s);
    return 0;
}
