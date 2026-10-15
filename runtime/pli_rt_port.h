/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM */
/* runtime/pli_rt_port.h — single translation point for platform conditionals
 * (ADR-221, runtime-reduction plan Phases 5, 7, 8, 9).
 *
 * Every rt_*.c file reaches this header transitively via pli_rt.h /
 * pli_rt_state.h and calls the pli_* wrappers instead of the raw stdlib /
 * POSIX / Windows APIs. Each wrapper carries exactly one platform branch at
 * its definition site, so no rt_*.c file needs its own #ifdef. All wrappers
 * are `static inline`: no exported symbols, no ABI surface, and the per-target
 * bitcode build (`-target <triple>`, which predefines _WIN32 for Windows
 * targets) inlines the correct branch for each host.
 *
 * Deliberately NOT wrapped here (no platform divergence to centralise):
 * - pli_memcpy/pli_memset/pli_strlen/... (ported in pli_rt_state.h, Phase 1&4)
 * - pli_strtoll/pli_strtod (ported in rt_get.c, Phase 2)
 * - pli_snprintf/pli_vsnprintf (ported in rt_stream.c, Phase 3)
 * - pli_exit/pli_abort (the exit/abort wrappers, Phase 6; pli_exit's body in
 *   rt_core.c calls raw exit() from the <stdlib.h> included below)
 * - __builtin_* math intrinsics (compiler builtins, not platform calls)
 * - stdout/stdin/stderr/EOF (standard handles/macros, identical everywhere)
 */
#ifndef PLI_RT_PORT_H
#define PLI_RT_PORT_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>

#if !defined(_WIN32)
#include <sys/wait.h> /* WIFEXITED/WEXITSTATUS for pli_pclose */
#endif

/* --- allocation (Phase 5) ------------------------------------------------- */
/* Thin forwards so a future allocator (e.g. an arena per ADR-079) or leak
 * tracking lands in one place instead of at every call site. There is no
 * pli_free here: the ABI's pli_free(char *) (FREE, rule (90), defined in
 * rt_storage.c) is the one free wrapper, reused for internal frees. */
static inline void *pli_malloc(size_t n) { return malloc(n); }
static inline void *pli_calloc(size_t n, size_t s) { return calloc(n, s); }
static inline void *pli_realloc(void *p, size_t n) { return realloc(p, n); }

/* --- process I/O: popen/pclose (Phase 8) ---------------------------------- */
/* pli_pclose returns the child exit status (0-255) or -1 on failure, hiding
 * the POSIX wait-status dance (WIFEXITED/WEXITSTATUS) from callers. */
#if defined(_WIN32)
static inline FILE *pli_popen(const char *cmd, const char *mode) { return _popen(cmd, mode); }
static inline int pli_pclose(FILE *fp) { return _pclose(fp); }
#else
static inline FILE *pli_popen(const char *cmd, const char *mode) { return popen(cmd, mode); }
static inline int pli_pclose(FILE *fp) {
  int raw = pclose(fp);
  if (raw == -1)
    return -1;
  return WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
}
#endif

/* --- time (Phase 7) -------------------------------------------------------- */
/* Thread-safe localtime into the caller-supplied struct tm; NULL on failure.
 * POSIX localtime_r and MSVC localtime_s take their arguments in opposite
 * order — the one divergence this wrapper exists to hide. time() itself is
 * standard C and stays a direct call. */
#if defined(_WIN32)
static inline struct tm *pli_localtime(const time_t *t, struct tm *out) {
  return localtime_s(out, t) == 0 ? out : NULL;
}
#else
static inline struct tm *pli_localtime(const time_t *t, struct tm *out) {
  return localtime_r(t, out);
}
#endif

/* --- FILE I/O (Phase 9) ---------------------------------------------------- */
/* Centralised stdio so mode/encoding differences live here. Record files
 * already pass "rb"/"wb" (no translation on either platform) and stream
 * files pass "r"/"w", so the wrapper forwards the caller-chosen mode. */
static inline FILE *pli_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static inline int pli_fclose(FILE *f) { return fclose(f); }
static inline size_t pli_fread(void *buf, size_t sz, size_t n, FILE *f) {
  return fread(buf, sz, n, f);
}
static inline size_t pli_fwrite(const void *buf, size_t sz, size_t n, FILE *f) {
  return fwrite(buf, sz, n, f);
}
static inline int pli_fgetc(FILE *f) { return fgetc(f); }
static inline int pli_getchar(void) { return getchar(); }
static inline int pli_fputc(int c, FILE *f) { return fputc(c, f); }
static inline int pli_fflush(FILE *f) { return fflush(f); }
static inline int pli_ferror(FILE *f) { return ferror(f); }

/* Variadic fprintf via vfprintf forwarding (C89-compatible, no compiler-
 * dependent __builtin_va_list ABI exposure in the embedded bitcode). */
static inline int pli_fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(f, fmt, ap);
  va_end(ap);
  return n;
}

#endif /* PLI_RT_PORT_H */
