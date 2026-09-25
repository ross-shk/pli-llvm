/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* rt_core.c — PL/I runtime library (libpli): lifecycle: init/fini/STOP. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>
#include <stdlib.h>

void pli_rt_init(void) {
  col = 0;
  items_on_line = 0;
  data_items = 0;
  data_value_next = 0;
  tok_semi = 0;
}

void pli_rt_fini(void) {
  if (col > 0) {
    fputc('\n', stdout);
    col = 0;
  }
  fflush(stdout);
}

/* Centralised process exit (Phase 6). */
void pli_exit(int code) { exit(code); }

void pli_stop(void) {
  pli_rt_fini();
  pli_exit(0);
}
