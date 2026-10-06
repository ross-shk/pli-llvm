/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for array_sort benchmark */
#include <stdio.h>

int main(void) {
    int i, j, n, tmp, sum;
    int arr[18000];

    n = 18000;
    /* Initialize reverse-sorted: arr[0]=n-1, arr[1]=n-2, ..., arr[n-1]=0
       matches PL/I arr(i) = n - i for i=1..n (1-based).
       Sum of 0..17999 = 161991000. */
    for (i = 0; i < n; i++) {
        arr[i] = n - 1 - i;
    }

    /* Bubble sort - reverse sorted input */
    for (i = 0; i < n - 1; i++) {
        for (j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }

    sum = 0;
    for (i = 0; i < n; i++) {
        sum = sum + arr[i];
    }

    printf("%d\n", sum);
    return 0;
}
