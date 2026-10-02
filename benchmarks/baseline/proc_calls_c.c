/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for proc_calls benchmark */
#include <stdio.h>

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fibonacci(int n) {
    int a = 0, b = 1, c, i;
    for (i = 2; i <= n; i++) {
        c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int sum_array(int n, int arr[]) {
    int i, sum = 0;
    for (i = 0; i < n; i++) {
        sum = sum + arr[i];
    }
    return sum;
}

int main(void) {
    int i, result, test_arr[20];

    for (i = 0; i < 20; i++) {
        test_arr[i] = i + 1;
    }

    result = 0;
    for (i = 1; i <= 12; i++) {
        result = result + factorial(i) + fibonacci(i);
    }

    result = result + sum_array(20, test_arr);

    printf("%d\n", result);
    return 0;
}
