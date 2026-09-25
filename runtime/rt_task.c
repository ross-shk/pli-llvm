/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* rt_task.c — PL/I runtime library (libpli): multitasking: EVENT/WAIT/DELAY/TASK. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* Multitasking (rules (79),(82),(83), QR2.8): EVENT flags are i32 words owned
 * by PL/I variables (0 incomplete, 1 complete), serialised behind one
 * mutex+cond. Concurrent list-directed PUT may interleave lines; that order
 * is implementation-defined in this stage. */
static pthread_mutex_t pli_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pli_ev_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t pli_task_mu = PTHREAD_MUTEX_INITIALIZER;
static long long pli_task_next = 1;

void pli_event_reset(char *ev) {
  if (!ev)
    return;
  pthread_mutex_lock(&pli_ev_mu);
  *(int *)ev = 0;
  pthread_mutex_unlock(&pli_ev_mu);
}

/* Task end: mark the event complete and wake every waiter. */
void pli_event_complete(char *ev) {
  if (!ev)
    return;
  pthread_mutex_lock(&pli_ev_mu);
  *(int *)ev = 1;
  pthread_cond_broadcast(&pli_ev_cv);
  pthread_mutex_unlock(&pli_ev_mu);
}

/* WAIT(ev): suspend until the event is complete. */
void pli_event_wait(char *ev) {
  if (!ev)
    return;
  pthread_mutex_lock(&pli_ev_mu);
  while (*(int *)ev == 0)
    pthread_cond_wait(&pli_ev_cv, &pli_ev_mu);
  pthread_mutex_unlock(&pli_ev_mu);
}

/* WAIT(evs)(k): evs is an array of n event addresses; suspend until at least
 * need of them are complete (need <= 0 returns at once, need > n waits all). */
void pli_wait_n(char *evs, long long n, long long need) {
  if (!evs || n <= 0 || need <= 0)
    return;
  if (need > n)
    need = n;
  pthread_mutex_lock(&pli_ev_mu);
  for (;;) {
    long long done = 0;
    char **addrs = (char **)evs;
    for (long long i = 0; i < n; ++i)
      if (addrs[i] && *(int *)addrs[i] != 0 && ++done >= need)
        break;
    if (done >= need)
      break;
    pthread_cond_wait(&pli_ev_cv, &pli_ev_mu);
  }
  pthread_mutex_unlock(&pli_ev_mu);
}

/* EVENT(ev) poll: 1 when complete, 0 otherwise. */
unsigned char pli_event_status(char *ev) {
  if (!ev)
    return 1;
  pthread_mutex_lock(&pli_ev_mu);
  int done = *(int *)ev != 0;
  pthread_mutex_unlock(&pli_ev_mu);
  return (unsigned char)(done ? 1 : 0);
}

/* DELAY(n): suspend for n milliseconds; n <= 0 is a no-op. */
void pli_delay(long long ms) {
  if (ms <= 0)
    return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    ;
}

/* CALL ... TASK/EVENT/PRIORITY: run wrapper(ctx) on a detached thread. */
void pli_task_spawn(char *fn, char *ctx) {
  void *(*body)(void *) = (void *(*)(void *))(void *)fn;
  pthread_t th;
  if (pthread_create(&th, NULL, body, ctx) != 0)
    pli_abort("TASK: could not create a thread");
  pthread_detach(th);
}

/* CALL ... TASK(t): give the task variable an observable handle id. */
void pli_task_note(char *task) {
  if (!task)
    return;
  pthread_mutex_lock(&pli_task_mu);
  *(int *)task = (int)(pli_task_next++);
  pthread_mutex_unlock(&pli_task_mu);
}

