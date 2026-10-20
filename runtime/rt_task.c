/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_task.c — PL/I runtime library (pli_runtime): multitasking: EVENT/WAIT/DELAY/TASK. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* Threading arrives via rt_sync.h (the runtime's lock layer, ADR-234): the
 * mutexes, conds, spawn and sleep are platform-TU definitions now, reached
 * through the layer that carries the fiber mode. */
#include "rt_sync.h"
#include "rt_sched.h"

/* Multitasking (rules (79),(82),(83), QR2.8): EVENT flags are i32 words owned
 * by PL/I variables (0 incomplete, 1 complete), serialised behind one
 * mutex+cond. Concurrent stream access is serialised by the stream locks
 * (ADR-233): each output statement is written whole and each input statement
 * reads whole records; the order in which the tasks' records appear is
 * implementation-defined. */
/* No static initializers: CRITICAL_SECTION/CONDITION_VARIABLE have none, so
 * both mutexes and the cond are created once via pli_once. */
/* One-time init: MSVC and clang-cl both lack static mutex/cond initializers. */
static rt_lock pli_ev_mu;
static rt_cond pli_ev_cv;
static rt_lock pli_task_mu;
static pli_once pli_rt_once = PLI_ONCE_INIT;
static long long pli_task_next = 1;

/* One-time creation of the EVENT mutex+cond and the task-id mutex. */
static void pli_rt_init_sync(void) {
  rt_lock_init(&pli_ev_mu);
  rt_cond_init(&pli_ev_cv);
  rt_lock_init(&pli_task_mu);
}

void pli_event_reset(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  *(int *)ev = 0;
  rt_lock_release(&pli_ev_mu);
}

/* Task end: mark the event complete and wake every waiter. */
void pli_event_complete(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  *(int *)ev = 1;
  /* Signal this event, not the whole table: a program with many events
   * would otherwise wake every waiter for every task that ends. */
  rt_cond_signal_key(&pli_ev_cv, ev);
  rt_lock_release(&pli_ev_mu);
}

/* WAIT(ev): suspend until the event is complete. */
void pli_event_wait(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  while (*(int *)ev == 0)
    rt_cond_wait_keyed(&pli_ev_cv, &pli_ev_mu, ev);
  rt_lock_release(&pli_ev_mu);
}

/* WAIT(evs)(k): evs is an array of n event addresses; suspend until at least
 * need of them are complete (need <= 0 returns at once, need > n waits all). */
void pli_wait_n(char *evs, long long n, long long need) {
  if (!evs || n <= 0 || need <= 0)
    return;
  if (need > n)
    need = n;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  for (;;) {
    long long done = 0;
    char **addrs = (char **)evs;
    for (long long i = 0; i < n; ++i)
      if (addrs[i] && *(int *)addrs[i] != 0 && ++done >= need)
        break;
    if (done >= need)
      break;
    rt_cond_wait(&pli_ev_cv, &pli_ev_mu);
  }
  rt_lock_release(&pli_ev_mu);
}

/* EVENT(ev) poll: 1 when complete, 0 otherwise. */
unsigned char pli_event_status(char *ev) {
  if (!ev)
    return 1;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  int done = *(int *)ev != 0;
  rt_lock_release(&pli_ev_mu);
  return (unsigned char)(done ? 1 : 0);
}

/* DELAY(n): suspend for n milliseconds. Under the fiber model this parks
 * the task with a timer deadline, so the worker runs other tasks while it
 * waits; under the legacy model it sleeps the OS thread. */
void pli_delay(long long ms) {
  if (ms <= 0)
    return;
  if (!rt_sched_fiber_mode() || !rt_sched_current()) {
    pli_sleep_ms(ms);
    return;
  }
  rt_sched_park_until(pli_plat_now_ms() + ms);
}

/* CALL ... TASK/EVENT/PRIORITY: start the callee. Under the fiber model it
 * runs as a task on the scheduler; under the legacy model it gets a
 * detached OS thread. */
void pli_task_spawn(char *fn, char *ctx) {
  /* The cast goes straight from the object pointer to the function pointer:
   * going through void * is not a conforming conversion. */
  void *(*body)(void *) = (void *(*)(void *))fn;
  /* ADR-233: the callee reads and writes the same stream state as the main
   * thread, so the stream path is serialised from here on. The switch is
   * flipped before the thread exists: the first spawn always comes from the
   * main thread (no task can exist before it), so no two threads ever race
   * on it. */
  rt_stream_enable_threads();
  if (rt_sched_fiber_mode()) {
    if (rt_sched_spawn(body, ctx) != 0)
      pli_abort("TASK: could not create a task");
    return;
  }
  if (pli_spawn_detached(body, ctx) != 0)
    pli_abort("TASK: could not create a thread");
}

/* CALL ... TASK(t): give the task variable an observable handle id. */
void pli_task_note(char *task) {
  if (!task)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_task_mu);
  *(int *)task = (int)(pli_task_next++);
  rt_lock_release(&pli_task_mu);
}

