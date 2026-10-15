/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM */
/* runtime/sync/pli_thread.h — portable threading abstraction (Phase 0, ADR-221). */
/* One identical C11 API over POSIX pthreads and MSVC primitives, so the
 * runtime compiles for ELF/Mach-O/COFF hosts without #ifdefs at use sites.
 * All functions are static inline; the header has no link-time footprint.
 * Compilable as C11; no C++ in the runtime.
 *
 * Platform selection is the single #if/#else block below: a Windows section
 * and a POSIX section, each carrying its own typedefs plus every function
 * body. No function carries its own #ifdef; the shared wrappers after the
 * #endif are platform-independent. */
#ifndef PLI_THREAD_H
#define PLI_THREAD_H

#if defined(_WIN32)
/* --- Windows backend (single selection block, first half) ------------------ */
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
typedef CRITICAL_SECTION pli_mutex;
typedef CONDITION_VARIABLE pli_cond;
typedef INIT_ONCE pli_once;
typedef uintptr_t pli_thread;
#define PLI_ONCE_INIT INIT_ONCE_STATIC_INIT

/* Mutual exclusion around one shared EVENT/task table. */
static inline void pli_mutex_init(pli_mutex *m) { InitializeCriticalSection(m); }
static inline void pli_mutex_lock(pli_mutex *m) { EnterCriticalSection(m); }
static inline void pli_mutex_unlock(pli_mutex *m) { LeaveCriticalSection(m); }

/* Condition broadcast/wait paired with the mutex above. */
static inline void pli_cond_init(pli_cond *c) { InitializeConditionVariable(c); }
static inline void pli_cond_wait(pli_cond *c, pli_mutex *m) {
  SleepConditionVariableCS(c, m, INFINITE);
}
static inline void pli_cond_signal(pli_cond *c) { WakeConditionVariable(c); }
static inline void pli_cond_broadcast(pli_cond *c) { WakeAllConditionVariable(c); }

/* One-time initialisation for mutexes/conds that have no static initializer. */
/* Adapter carrying a `void(*)(void)` through InitOnceExecuteOnce. */
struct pli_once_ctx {
  void (*fn)(void);
};
static inline BOOL CALLBACK pli_once_trampoline(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o;
  (void)c;
  ((struct pli_once_ctx *)p)->fn();
  return TRUE;
}
static inline void pli_call_once(pli_once *o, void (*fn)(void)) {
  /* Synchronous: the stack argument outlives the call. */
  struct pli_once_ctx ctx;
  ctx.fn = fn;
  InitOnceExecuteOnce(o, pli_once_trampoline, &ctx, NULL);
}

/* Spawn a detached thread running fn(arg); returns 0 on success. */
/* Heap pair carrying `void *(*)(void *)` through _beginthreadex. */
/* _beginthreadex needs `unsigned __stdcall`; carry fn+arg on the heap. */
struct pli_spawn_ctx {
  void *(*fn)(void *);
  void *arg;
};
static inline unsigned __stdcall pli_spawn_trampoline(void *p) {
  struct pli_spawn_ctx *c = (struct pli_spawn_ctx *)p;
  void *(*fn)(void *) = c->fn;
  void *arg = c->arg;
  free(c);
  fn(arg);
  _endthreadex(0);
  return 0;
}
static inline int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg) {
  struct pli_spawn_ctx *c = (struct pli_spawn_ctx *)malloc(sizeof(*c));
  if (!c)
    return -1;
  c->fn = fn;
  c->arg = arg;
  uintptr_t h =
      _beginthreadex(NULL, 0, pli_spawn_trampoline, c, 0, NULL);
  if (h == 0) {
    free(c);
    return -1;
  }
  *th = h;
  return 0;
}
static inline void pli_thread_detach(pli_thread th) { CloseHandle((HANDLE)th); }

/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
static inline void pli_sleep_ms(long long ms) {
  if (ms <= 0)
    return;
  Sleep((DWORD)ms);
}

#else
/* --- POSIX backend (single selection block, second half) ------------------- */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
typedef pthread_mutex_t pli_mutex;
typedef pthread_cond_t pli_cond;
typedef pthread_once_t pli_once;
typedef pthread_t pli_thread;
#define PLI_ONCE_INIT PTHREAD_ONCE_INIT

/* Mutual exclusion around one shared EVENT/task table. */
static inline void pli_mutex_init(pli_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void pli_mutex_lock(pli_mutex *m) { pthread_mutex_lock(m); }
static inline void pli_mutex_unlock(pli_mutex *m) { pthread_mutex_unlock(m); }

/* Condition broadcast/wait paired with the mutex above. */
static inline void pli_cond_init(pli_cond *c) { pthread_cond_init(c, NULL); }
static inline void pli_cond_wait(pli_cond *c, pli_mutex *m) { pthread_cond_wait(c, m); }
static inline void pli_cond_signal(pli_cond *c) { pthread_cond_signal(c); }
static inline void pli_cond_broadcast(pli_cond *c) { pthread_cond_broadcast(c); }

/* One-time initialisation for mutexes/conds that have no static initializer. */
static inline void pli_call_once(pli_once *o, void (*fn)(void)) { pthread_once(o, fn); }

/* Spawn a detached thread running fn(arg); returns 0 on success. */
static inline int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg) {
  return pthread_create(th, NULL, fn, arg);
}
static inline void pli_thread_detach(pli_thread th) { pthread_detach(th); }

/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
static inline void pli_sleep_ms(long long ms) {
  struct timespec ts;
  if (ms <= 0)
    return;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    ;
}
#endif

/* --- shared wrappers (platform-independent, no conditionals) -------------- */
/* Spawn a detached thread running fn(arg); returns 0 on success. */
static inline int pli_spawn_detached(void *(*fn)(void *), void *arg) {
  pli_thread th;
  if (pli_spawn(&th, fn, arg) != 0)
    return -1;
  pli_thread_detach(th);
  return 0;
}

#endif /* PLI_THREAD_H */
