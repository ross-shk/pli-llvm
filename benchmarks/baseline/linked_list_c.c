/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for linked_list benchmark */
#include <stdio.h>
#include <stdlib.h>

typedef struct node {
    int val;
    struct node *next;
} node_t;

int main(void) {
    int i, n, sum;
    node_t *p, *head;

    n = 5000;
    head = NULL;

    /* Build linked list: 0 -> 1 -> 2 -> ... -> n-1 */
    for (i = 0; i < n; i++) {
        p = (node_t *)malloc(sizeof(node_t));
        p->val = i;
        p->next = head;
        head = p;
    }

    /* Traverse and sum */
    sum = 0;
    p = head;
    while (p != NULL) {
        sum = sum + p->val;
        p = p->next;
    }

    /* Free all nodes */
    for (i = 0; i < n; i++) {
        if (head != NULL) {
            p = head;
            head = p->next;
            free(p);
        }
    }

    printf("%d\n", sum);
    return 0;
}
