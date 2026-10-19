/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* runtime/rt_plat_default.c — default platform TU: the host half of the split
 * runtime. Implements every symbol declared in pli_rt_plat.h against the host
 * libc/pthreads (or Win32), so the portable TUs stay branch-free. Bodies are
 * the former pli_rt_port.h wrappers (ADR-221) plus the sync/pli_thread.h
 * backends, moved out-of-line unchanged. This is the TU --rt-platform
 * replaces: a custom platform provides these same symbols and the portable
 * blob links against it untouched. Self-contained by contract: names only
 * libc and pli_rt_plat.h here. */
#include "pli_rt_plat.h"

#include <stdarg.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

/* --- allocation: direct libc (the portable TUs must not name these) */
void *pli_plat_malloc(size_t n) { return malloc(n); }
void *pli_plat_calloc(size_t n, size_t size) { return calloc(n, size); }
void *pli_plat_realloc(void *p, size_t n) { return realloc(p, n); }
void pli_plat_free(void *p) { free(p); }

/* --- exit/abort raw ends (flushing happens in portable pli_exit/pli_abort) */
void pli_plat_exit(int code) { exit(code); }
void pli_plat_abort(void) { abort(); }

/* --- process spawn: one branch per platform, wait-status dance included */
FILE *pli_plat_popen(const char *cmd, const char *mode) {
#if defined(_WIN32)
  return _popen(cmd, mode);
#else
  return popen(cmd, mode);
#endif
}
int pli_plat_pclose(FILE *fp) {
#if defined(_WIN32)
  return _pclose(fp);
#else
  int raw = pclose(fp);
  if (raw == -1)
    return -1;
  if (WIFEXITED(raw))
    return WEXITSTATUS(raw);
  return -1;
#endif
}

/* --- calendar time: re-entrant POSIX, locked _s on Windows */
void pli_plat_localtime(const time_t *t, struct tm *out) {
#if defined(_WIN32)
  localtime_s(out, t);
#else
  localtime_r(t, out);
#endif
}

/* --- FILE I/O: mode strings ("r"/"w" vs "rb"/"wb") chosen by the callers */
FILE *pli_plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int pli_plat_fclose(FILE *fp) { return fclose(fp); }
size_t pli_plat_fread(void *p, size_t size, size_t n, FILE *fp) {
  return fread(p, size, n, fp);
}
size_t pli_plat_fwrite(const void *p, size_t size, size_t n, FILE *fp) {
  return fwrite(p, size, n, fp);
}
int pli_plat_fgetc(FILE *fp) { return fgetc(fp); }
int pli_plat_getchar(void) { return getchar(); }
int pli_plat_fputc(int c, FILE *fp) { return fputc(c, fp); }
int pli_plat_fflush(FILE *fp) { return fflush(fp); }
int pli_plat_ferror(FILE *fp) { return ferror(fp); }
/* Printf via vfprintf: the ... crosses the seam, so __builtin_va_list never
 * appears in the portable blob's ABI. */
int pli_plat_fprintf(FILE *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(fp, fmt, ap);
  va_end(ap);
  return n;
}

/* --- threading backends (single selection block; bodies ex pli_thread.h) */
#if defined(_WIN32)
/* Windows backend (first half). */
#include <process.h>
/* Mutual exclusion around one shared EVENT/task table. */
void pli_mutex_init(pli_mutex *m) { InitializeCriticalSection(m); }
void pli_mutex_lock(pli_mutex *m) { EnterCriticalSection(m); }
void pli_mutex_unlock(pli_mutex *m) { LeaveCriticalSection(m); }
/* Recursive mutex: a CRITICAL_SECTION is recursive by nature. */
void pli_mutex_init_recursive(pli_mutex *m) { InitializeCriticalSection(m); }
/* Condition broadcast/wait paired with the mutex above. */
void pli_cond_init(pli_cond *c) { InitializeConditionVariable(c); }
void pli_cond_wait(pli_cond *c, pli_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
void pli_cond_signal(pli_cond *c) { WakeConditionVariable(c); }
void pli_cond_broadcast(pli_cond *c) { WakeAllConditionVariable(c); }
/* One-time initialisation for mutexes/conds that have no static initializer. */
/* Adapter carrying a `void(*)(void)` through InitOnceExecuteOnce. */
struct pli_once_ctx {
  void (*fn)(void);
};
static BOOL CALLBACK pli_once_trampoline(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o;
  (void)c;
  ((struct pli_once_ctx *)p)->fn();
  return TRUE;
}
void pli_call_once(pli_once *o, void (*fn)(void)) {
  /* Synchronous: the stack argument outlives the call. */
  struct pli_once_ctx ctx;
  ctx.fn = fn;
  InitOnceExecuteOnce(o, pli_once_trampoline, &ctx, NULL);
}
/* Spawn a thread running fn(arg); returns 0 on success. */
/* Heap pair carrying `void *(*)(void *)` through _beginthreadex. */
/* _beginthreadex needs `unsigned __stdcall`; carry fn+arg on the heap. */
struct pli_spawn_ctx {
  void *(*fn)(void *);
  void *arg;
};
static unsigned __stdcall pli_spawn_trampoline(void *p) {
  struct pli_spawn_ctx *c = (struct pli_spawn_ctx *)p;
  void *(*fn)(void *) = c->fn;
  void *arg = c->arg;
  free(c);
  fn(arg);
  _endthreadex(0);
  return 0;
}
int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg) {
  struct pli_spawn_ctx *c = (struct pli_spawn_ctx *)malloc(sizeof(*c));
  if (!c)
    return -1;
  c->fn = fn;
  c->arg = arg;
  uintptr_t h = _beginthreadex(NULL, 0, pli_spawn_trampoline, c, 0, NULL);
  if (h == 0) {
    free(c);
    return -1;
  }
  *th = h;
  return 0;
}
void pli_thread_detach(pli_thread th) { CloseHandle((HANDLE)th); }
/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
void pli_sleep_ms(long long ms) {
  if (ms <= 0)
    return;
  Sleep((DWORD)ms);
}
#else
/* POSIX backend (second half). */
#include <errno.h>
#include <unistd.h>
/* Mutual exclusion around one shared EVENT/task table. */
void pli_mutex_init(pli_mutex *m) { pthread_mutex_init(m, NULL); }
void pli_mutex_lock(pli_mutex *m) { pthread_mutex_lock(m); }
void pli_mutex_unlock(pli_mutex *m) { pthread_mutex_unlock(m); }
/* Recursive mutex: the output serialiser takes its lock nested. */
void pli_mutex_init_recursive(pli_mutex *m) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
}
/* Condition broadcast/wait paired with the mutex above. */
void pli_cond_init(pli_cond *c) { pthread_cond_init(c, NULL); }
void pli_cond_wait(pli_cond *c, pli_mutex *m) { pthread_cond_wait(c, m); }
void pli_cond_signal(pli_cond *c) { pthread_cond_signal(c); }
void pli_cond_broadcast(pli_cond *c) { pthread_cond_broadcast(c); }
/* One-time initialisation for mutexes/conds that have no static initializer. */
void pli_call_once(pli_once *o, void (*fn)(void)) { pthread_once(o, fn); }
/* Spawn a thread running fn(arg); returns 0 on success. */
int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg) {
  return pthread_create(th, NULL, fn, arg);
}
void pli_thread_detach(pli_thread th) { pthread_detach(th); }
/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
void pli_sleep_ms(long long ms) {
  struct timespec ts;
  if (ms <= 0)
    return;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    ;
}
#endif

/* --- shared wrapper (platform-independent; was the header's tail) */
/* Spawn a detached thread running fn(arg); returns 0 on success. */
int pli_spawn_detached(void *(*fn)(void *), void *arg) {
  pli_thread th;
  if (pli_spawn(&th, fn, arg) != 0)
    return -1;
  pli_thread_detach(th);
  return 0;
}
