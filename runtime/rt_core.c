/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_core.c — PL/I runtime library (pli_runtime): lifecycle: init/fini/STOP. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* System headers arrive via pli_rt_plat.h (the platform seam): allocation,
 * process I/O, and stdio all route through the pli_plat_* wrappers below. */

/* Version stamp for the bitcode runtime (OPTIMIZATION.md §10, P3): runtime.bc
 * is compiled by the same LLVM as pli, and IRGen checks this global before
 * linking it into a module. A stale bitcode would silently mis-compile, so a
 * missing or mismatched stamp is diagnosed instead. */
#ifndef PLI_LLVM_VERSION
#define PLI_LLVM_VERSION "unknown"
#endif
const char pli_llvm_version[] = PLI_LLVM_VERSION;

void pli_rt_init(void) {
  col = 0;
  items_on_line = 0;
  data_items = 0;
  data_value_next = 0;
  tok_semi = 0;
}

void pli_rt_fini(void) {
  /* ADR-220: flush buffered stdout before the trailing-newline fputc so
   * the final newline lands after the buffered content, not before it. */
  rt_flush_stdout();
  if (col > 0) {
    pli_plat_fputc('\n', stdout);
    col = 0;
  }
  pli_plat_fflush(stdout);
}

/* Centralised process exit (Phase 6): the one exit path; the raw exit() call
 * lives in the platform TU (pli_plat_exit) so no portable TU names it. */
void pli_exit(int code) { pli_plat_exit(code); }

void pli_stop(void) {
  pli_rt_fini();
  pli_exit(0);
}

int pli_system(const char *cmd, long long len) {
  char *buf = (char *)pli_plat_malloc((size_t)len + 1);
  if (!buf) {
    return -1;
  }
  pli_memcpy(buf, cmd, (size_t)len);
  buf[len] = '\0';
  /* Platform popen/pclose plus the wait-status dance hide in pli_plat_pclose. */
  FILE *fp = pli_plat_popen(buf, "r");
  int status = -1;
  if (fp) {
    status = pli_plat_pclose(fp);
  }
  pli_free(buf);
  return status;
}

/* EMPTY() (ADR-200): returns a null AREA handle. The AREA is a pointer;
   an empty/null AREA is simply NULL. */
char *pli_empty_area(void) {
  return NULL;
}
