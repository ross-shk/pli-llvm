/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* runtime/sync/plic_thread.h — portable threading abstraction (Phase 0). */
/* One identical C11 API over POSIX pthreads and MSVC primitives, so the
 * runtime compiles for ELF/Mach-O/COFF hosts without #ifdefs at use sites.
 * All functions are static inline; the header has no link-time footprint. */
#ifndef PLIC_THREAD_H
#define PLIC_THREAD_H

#if defined(_WIN32)
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
typedef CRITICAL_SECTION pli_mutex;
typedef CONDITION_VARIABLE pli_cond;
typedef INIT_ONCE pli_once;
typedef uintptr_t pli_thread;
#define PLI_ONCE_INIT INIT_ONCE_STATIC_INIT
#else
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
#endif

/* Mutual exclusion around one shared EVENT/task table. */
static inline void pli_mutex_init(pli_mutex *m) {
#if defined(_WIN32)
  InitializeCriticalSection(m);
#else
  pthread_mutex_init(m, NULL);
#endif
}

static inline void pli_mutex_lock(pli_mutex *m) {
#if defined(_WIN32)
  EnterCriticalSection(m);
#else
  pthread_mutex_lock(m);
#endif
}

static inline void pli_mutex_unlock(pli_mutex *m) {
#if defined(_WIN32)
  LeaveCriticalSection(m);
#else
  pthread_mutex_unlock(m);
#endif
}

/* Condition broadcast/wait paired with the mutex above. */
static inline void pli_cond_init(pli_cond *c) {
#if defined(_WIN32)
  InitializeConditionVariable(c);
#else
  pthread_cond_init(c, NULL);
#endif
}

static inline void pli_cond_wait(pli_cond *c, pli_mutex *m) {
#if defined(_WIN32)
  SleepConditionVariableCS(c, m, INFINITE);
#else
  pthread_cond_wait(c, m);
#endif
}

static inline void pli_cond_broadcast(pli_cond *c) {
#if defined(_WIN32)
  WakeAllConditionVariable(c);
#else
  pthread_cond_broadcast(c);
#endif
}

/* One-time initialisation for mutexes/conds that have no static initializer. */
#if defined(_WIN32)
/* Adapter from InitOnceExecuteOnce's signature to a plain void(void) fn. */
struct pli_once_arg {
  void (*fn)(void);
};
static BOOL CALLBACK pli_once_tramp(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o;
  (void)c;
  ((struct pli_once_arg *)p)->fn();
  return TRUE;
}
#endif

static inline void pli_call_once(pli_once *o, void (*fn)(void)) {
#if defined(_WIN32)
  /* Synchronous: the stack argument outlives the call. */
  struct pli_once_arg a;
  a.fn = fn;
  InitOnceExecuteOnce(o, pli_once_tramp, &a, NULL);
#else
  pthread_once(o, fn);
#endif
}

/* Spawn a detached thread running fn(arg); returns 0 on success. */
#if defined(_WIN32)
/* _beginthreadex needs `unsigned __stdcall`; carry fn+arg on the heap. */
struct pli_spawn_arg {
  void *(*fn)(void *);
  void *arg;
};
static unsigned __stdcall pli_spawn_tramp(void *p) {
  struct pli_spawn_arg *a = (struct pli_spawn_arg *)p;
  void *(*fn)(void *) = a->fn;
  void *arg = a->arg;
  free(a);
  fn(arg);
  return 0;
}
#endif

static inline int pli_spawn_detached(void *(*fn)(void *), void *arg) {
#if defined(_WIN32)
  struct pli_spawn_arg *a = (struct pli_spawn_arg *)malloc(sizeof *a);
  uintptr_t h;
  if (!a)
    return -1;
  a->fn = fn;
  a->arg = arg;
  h = _beginthreadex(NULL, 0, pli_spawn_tramp, a, 0, NULL);
  if (h == 0) {
    free(a);
    return -1;
  }
  CloseHandle((HANDLE)h);
  return 0;
#else
  pthread_t th;
  if (pthread_create(&th, NULL, fn, arg) != 0)
    return -1;
  pthread_detach(th);
  return 0;
#endif
}

/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
static inline void pli_sleep_ms(long long ms) {
  if (ms <= 0)
    return;
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    ;
#endif
}

#endif /* PLIC_THREAD_H */
