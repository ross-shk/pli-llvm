/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* Included file for preproc_edge test */
   %declare inc_var;
   %inc_var = 777;
   %pp_count = pp_count + inc_var;