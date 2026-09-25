/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* Provides C callees taking scalars by value (rule (34), C ABI): the
   PL/I side declares matching ENTRYs with OPTIONS(LINKAGE(SYSTEM)) or
   OPTIONS(BYVALUE) and EXTERNAL names. */
int c_byval_add(int a, int b) { return a * 1000 + b; }
int c_byval_scale(int *p, int scale) { return (*p) * scale; }
double c_byval_double(double x) { return x * 2.0; }
void c_byval_touch(int *p) { *p = 7; }
