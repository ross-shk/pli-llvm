/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* C baseline for linked_list benchmark */
#include <stdio.h>
#include <stdlib.h>

typedef struct node {
    int val;
    struct node *next;
} node_t;

int main(void) {
    int i, n;
    long long sum;
    node_t *p, *head;

     n = 3000000;
    head = NULL;

    /* Build linked list: 0 -> 1 -> 2 -> ... -> n-1 */
    for (i = 0; i < n; i++) {
        p = (node_t *)malloc(sizeof(node_t));
        /* PL/I raises an ALLOCATE condition on failure; the C twin must
           check the same thing rather than dereferencing NULL. */
        if (p == NULL) {
            fprintf(stderr, "ALLOCATE failed at %d\n", i);
            return 1;
        }
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

    printf("%lld\n", sum);
    return 0;
}
