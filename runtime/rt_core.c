/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* rt_core.c — PL/I runtime library (libpli): lifecycle: init/fini/STOP. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

/* Version stamp for the bitcode runtime (OPTIMIZATION.md §10, P3): runtime.bc
 * is compiled by the same LLVM as plic, and IRGen checks this global before
 * linking it into a module. A stale bitcode would silently mis-compile, so a
 * missing or mismatched stamp is diagnosed instead. */
#ifndef PLIC_LLVM_VERSION
#define PLIC_LLVM_VERSION "unknown"
#endif
const char plic_llvm_version[] = PLIC_LLVM_VERSION;

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

int pli_system(const char *cmd, long long len) {
  char *buf = (char *)malloc((size_t)len + 1);
  if (!buf) {
    return -1;
  }
  memcpy(buf, cmd, (size_t)len);
  buf[len] = '\0';
#if defined(_WIN32)
  FILE *fp = _popen(buf, "r");
#else
  FILE *fp = popen(buf, "r");
#endif
  int status = -1;
  if (fp) {
#if defined(_WIN32)
    status = _pclose(fp);
#else
    int raw = pclose(fp);
    status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
  }
  free(buf);
  return status;
}
