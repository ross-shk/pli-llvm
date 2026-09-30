/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* rt_task.c — PL/I runtime library (libpli): multitasking: EVENT/WAIT/DELAY/TASK. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include "sync/plic_thread.h"

/* Multitasking (rules (79),(82),(83), QR2.8): EVENT flags are i32 words owned
 * by PL/I variables (0 incomplete, 1 complete), serialised behind one
 * mutex+cond. Concurrent list-directed PUT may interleave lines; that order
 * is implementation-defined in this stage. */
static pli_mutex pli_ev_mu;
static pli_cond pli_ev_cv;
static pli_mutex pli_task_mu;
static pli_once pli_rt_once = PLI_ONCE_INIT;
static long long pli_task_next = 1;

/* One-time init: MSVC has no static mutex/cond initializers. */
static void pli_rt_init_sync(void) {
  pli_mutex_init(&pli_ev_mu);
  pli_cond_init(&pli_ev_cv);
  pli_mutex_init(&pli_task_mu);
}

void pli_event_reset(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  pli_mutex_lock(&pli_ev_mu);
  *(int *)ev = 0;
  pli_mutex_unlock(&pli_ev_mu);
}

/* Task end: mark the event complete and wake every waiter. */
void pli_event_complete(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  pli_mutex_lock(&pli_ev_mu);
  *(int *)ev = 1;
  pli_cond_broadcast(&pli_ev_cv);
  pli_mutex_unlock(&pli_ev_mu);
}

/* WAIT(ev): suspend until the event is complete. */
void pli_event_wait(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  pli_mutex_lock(&pli_ev_mu);
  while (*(int *)ev == 0)
    pli_cond_wait(&pli_ev_cv, &pli_ev_mu);
  pli_mutex_unlock(&pli_ev_mu);
}

/* WAIT(evs)(k): evs is an array of n event addresses; suspend until at least
 * need of them are complete (need <= 0 returns at once, need > n waits all). */
void pli_wait_n(char *evs, long long n, long long need) {
  if (!evs || n <= 0 || need <= 0)
    return;
  if (need > n)
    need = n;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  pli_mutex_lock(&pli_ev_mu);
  for (;;) {
    long long done = 0;
    char **addrs = (char **)evs;
    for (long long i = 0; i < n; ++i)
      if (addrs[i] && *(int *)addrs[i] != 0 && ++done >= need)
        break;
    if (done >= need)
      break;
    pli_cond_wait(&pli_ev_cv, &pli_ev_mu);
  }
  pli_mutex_unlock(&pli_ev_mu);
}

/* EVENT(ev) poll: 1 when complete, 0 otherwise. */
unsigned char pli_event_status(char *ev) {
  if (!ev)
    return 1;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  pli_mutex_lock(&pli_ev_mu);
  int done = *(int *)ev != 0;
  pli_mutex_unlock(&pli_ev_mu);
  return (unsigned char)(done ? 1 : 0);
}

/* DELAY(n): suspend for n milliseconds; n <= 0 is a no-op. */
void pli_delay(long long ms) {
  pli_sleep_ms(ms);
}

/* CALL ... TASK/EVENT/PRIORITY: run wrapper(ctx) on a detached thread. */
void pli_task_spawn(char *fn, char *ctx) {
  void *(*body)(void *) = (void *(*)(void *))(void *)fn;
  if (pli_spawn_detached(body, ctx) != 0)
    pli_abort("TASK: could not create a thread");
}

/* CALL ... TASK(t): give the task variable an observable handle id. */
void pli_task_note(char *task) {
  if (!task)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  pli_mutex_lock(&pli_task_mu);
  *(int *)task = (int)(pli_task_next++);
  pli_mutex_unlock(&pli_task_mu);
}

