/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* Provides the external C function em_add that entry_mixed.pli declares as an
   ENTRY(...) RETURNS(...) function (rule (34)). PL/I passes each scalar
   argument by reference, so this receives pointers to the caller's variables
   and returns a value by C's normal function-result convention. */
int em_add(int *a, int *b) { return *a + *b; }
