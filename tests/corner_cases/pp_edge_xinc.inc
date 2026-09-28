/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* XINCLUDE file for preproc_edge test - only included once */
   %declare xinc_var;
   %xinc_var = 888;
   %pp_count = pp_count + xinc_var;