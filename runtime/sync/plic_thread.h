/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* plic_thread.h — portable threading abstraction (Phase 0).
 *
 * One identical API over POSIX pthreads and MSVC threads. Compilable as C11;
 * no C++ in the runtime. Windows branches are `#if`-compiled but exercised
 * on MSVC hosts/CI only.
 */
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

/* --- mutex -------------------------------------------------------------- */
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

/* --- condition variable -------------------------------------------------- */
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

static inline void pli_cond_signal(pli_cond *c) {
#if defined(_WIN32)
  WakeConditionVariable(c);
#else
  pthread_cond_signal(c);
#endif
}

static inline void pli_cond_broadcast(pli_cond *c) {
#if defined(_WIN32)
  WakeAllConditionVariable(c);
#else
  pthread_cond_broadcast(c);
#endif
}

/* --- once ---------------------------------------------------------------- */
#if defined(_WIN32)
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
#endif

static inline void pli_call_once(pli_once *o, void (*fn)(void)) {
#if defined(_WIN32)
  struct pli_once_ctx ctx;
  ctx.fn = fn;
  InitOnceExecuteOnce(o, pli_once_trampoline, &ctx, NULL);
#else
  pthread_once(o, fn);
#endif
}

/* --- threads -------------------------------------------------------------- */
#if defined(_WIN32)
/* Heap pair carrying `void *(*)(void *)` through _beginthreadex. */
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
#endif

static inline int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg) {
#if defined(_WIN32)
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
#else
  return pthread_create(th, NULL, fn, arg);
#endif
}

static inline void pli_thread_detach(pli_thread th) {
#if defined(_WIN32)
  CloseHandle((HANDLE)th);
#else
  pthread_detach(th);
#endif
}

static inline int pli_spawn_detached(void *(*fn)(void *), void *arg) {
  pli_thread th;
  if (pli_spawn(&th, fn, arg) != 0)
    return -1;
  pli_thread_detach(th);
  return 0;
}

/* --- sleep ----------------------------------------------------------------- */
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
