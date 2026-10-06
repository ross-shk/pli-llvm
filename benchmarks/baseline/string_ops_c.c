/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for string_ops benchmark */
#include <stdio.h>
#include <string.h>

int main(void) {
    int i, len;
    char base[1001], temp[101], result[180001], substr_result[6];

    strcpy(base, "Hello, PL/I! ");
    result[0] = '\0';

    for (i = 0; i < 8000; i++) {
        snprintf(temp, sizeof(temp), "%s[%sitem%s] ", base, "", "");
        strcat(result, temp);
    }

    len = strlen(result);
    for (i = 0; i < 100; i++) {
        int start = i * 10;
        if (start + 5 <= len) {
            memcpy(substr_result, result + start, 5);
            substr_result[5] = '\0';
        }
    }

    printf("%d\n", len);
    return 0;
}
