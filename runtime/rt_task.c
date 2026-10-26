/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_task.c — PL/I runtime library (pli_runtime): multitasking: EVENT/WAIT/DELAY/TASK. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdatomic.h>
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
static int pli_task_next = 1;

/* Live OS-model tasks (ADR-298). The legacy model detaches every task, so
 * without this count the process can exit while a task thread is still in
 * the runtime; pli_rt_fini waits for the count to reach zero first. */
static _Atomic int pli_os_live = 0;
static _Thread_local int pli_in_task = 0;

/* The legacy task trampoline: run the body, then report the task done. */
typedef struct {
  void *(*body)(void *);
  void *ctx;
} pli_os_call;

static void *pli_os_thread(void *p) {
  pli_os_call *c = (pli_os_call *)p;
  void *(*body)(void *) = c->body;
  void *ctx = c->ctx;
  pli_plat_free(c);
  pli_in_task = 1;
  body(ctx);
  pli_in_task = 0;
  atomic_fetch_sub_explicit(&pli_os_live, 1, memory_order_acq_rel);
  return NULL;
}

/* Wait for the legacy tasks to finish (no-op on the fiber model or inside
 * a task, where waiting for oneself would deadlock). */
void rt_wait_tasks(void) {
  if (rt_sched_fiber_mode() || pli_in_task)
    return;
  /* Bounded: a program that leaves a task blocked must still terminate. */
  for (int ms = 0; ms < 2000; ++ms) {
    if (atomic_load_explicit(&pli_os_live, memory_order_acquire) == 0)
      return;
    pli_sleep_ms(1);
  }
}

/* Task handle registry (ADR-298): maps a 1-based task id to its absolute
 * priority and the fiber currently running it. Ids are never recycled, so a
 * completed handle cannot alias a later-spawned task; the fiber pointer is
 * guarded by the fiber's own handle when a priority is updated. The array
 * grows on demand. */
typedef struct {
  int priority;      /* absolute priority (main task = 0) */
  pli_fiber *fiber;  /* the fiber running this handle, or NULL */
} pli_task_entry;
static pli_task_entry *pli_task_table = NULL;
static int pli_task_cap = 0;

/* Grow the registry to hold at least id (1-based). Returns NULL on OOM
 * (the caller treats that as "no priority recorded"). */
static pli_task_entry *pli_task_entry_for(long long id) {
  if (id < 1)
    return NULL;
  int idx = (int)(id - 1);
  if (idx >= pli_task_cap) {
    int cap = pli_task_cap ? pli_task_cap * 2 : 16;
    if (idx >= cap)
      cap = idx + 16;
    pli_task_entry *t = (pli_task_entry *)pli_plat_realloc(
        pli_task_table, (size_t)cap * sizeof(pli_task_entry));
    if (!t)
      return NULL;
    for (int i = pli_task_cap; i < cap; ++i) {
      t[i].priority = 0;
      t[i].fiber = NULL;
    }
    pli_task_table = t;
    pli_task_cap = cap;
  }
  return &pli_task_table[idx];
}

/* One-time creation of the EVENT mutex+cond and the task-id mutex. */
static void pli_rt_init_sync(void) {
  rt_lock_init(&pli_ev_mu);
  rt_cond_init(&pli_ev_cv);
  rt_lock_init(&pli_task_mu);
}

/* Already complete: no table lock. The completion's release store and this
 * acquire load give a waiter the same visibility the lock used to. */
static int rt_ev_done(const char *ev) {
  return pli_plat_load_i32_acquire(ev) != 0;
}

void pli_event_reset(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  pli_plat_store_i32_release(ev, 0);
  rt_lock_release(&pli_ev_mu);
}

/* Task end: mark the event complete and wake every waiter. */
void pli_event_complete(char *ev) {
  if (!ev)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  pli_plat_store_i32_release(ev, 1);
  /* Signal this event, not the whole table: a program with many events
   * would otherwise wake every waiter for every task that ends. */
  rt_cond_signal_key(&pli_ev_cv, ev);
  rt_lock_release(&pli_ev_mu);
}

/* WAIT(ev): suspend until the event is complete. */
void pli_event_wait(char *ev) {
  if (!ev)
    return;
  /* The fan-in common case: the task has already completed. */
  if (rt_ev_done(ev))
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  while (!rt_ev_done(ev))
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
  char **addrs = (char **)evs;
  /* Already satisfied: no table lock (the usual case in a fan-in). */
  long long done = 0;
  for (long long i = 0; i < n; ++i)
    if (addrs[i] && rt_ev_done(addrs[i]) && ++done >= need)
      return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_ev_mu);
  for (;;) {
    done = 0;
    for (long long i = 0; i < n; ++i)
      if (addrs[i] && rt_ev_done(addrs[i]) && ++done >= need)
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
  /* A poll needs one atomic load, not the table lock. */
  return (unsigned char)(rt_ev_done(ev) ? 1 : 0);
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

/* Start a detached OS-model task through the done-tracking trampoline. */
static void pli_spawn_task(void *(*body)(void *), void *ctx) {
  pli_os_call *c = (pli_os_call *)pli_plat_malloc(sizeof(pli_os_call));
  if (!c)
    pli_abort("TASK: out of memory");
  c->body = body;
  c->ctx = ctx;
  atomic_fetch_add_explicit(&pli_os_live, 1, memory_order_acq_rel);
  if (pli_spawn_detached(pli_os_thread, c) != 0) {
    atomic_fetch_sub_explicit(&pli_os_live, 1, memory_order_acq_rel);
    pli_plat_free(c);
    pli_abort("TASK: could not create a thread");
  }
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
  pli_spawn_task(body, ctx);
}

/* Priority-aware spawn (ADR-298): pli_task_spawn with a priority.
 * `id` is the task handle id (0 = no TASK variable, bare TASK); `prio` is
 * relative to the spawning task, so the stored value is absolute. */
void pli_task_spawn_p(char *fn, char *ctx, long long id, int prio) {
  void *(*body)(void *) = (void *(*)(void *))fn;
  rt_stream_enable_threads();
  int abs = prio + rt_sched_current_priority();
  /* Store the priority in the handle's registry entry. If there is no
   * handle (id == 0) the priority still affects scheduling of the fiber. */
  if (id > 0) {
    pli_call_once(&pli_rt_once, pli_rt_init_sync);
    rt_lock_acquire(&pli_task_mu);
    pli_task_entry *e = pli_task_entry_for(id);
    if (e)
      e->priority = abs;
    rt_lock_release(&pli_task_mu);
  }
  if (rt_sched_fiber_mode()) {
    pli_fiber *f = rt_sched_spawn_p(body, ctx, abs, id);
    if (!f)
      pli_abort("TASK: could not create a task");
    /* Publish the fiber so a later PRIORITY(t) can update a live task. */
    if (id > 0) {
      rt_lock_acquire(&pli_task_mu);
      pli_task_entry *e = pli_task_entry_for(id);
      if (e)
        e->fiber = f;
      rt_lock_release(&pli_task_mu);
    }
    return;
  }
  pli_spawn_task(body, ctx);
}

/* CALL ... TASK(t): give the task variable an observable handle id and
 * create its registry entry. */
void pli_task_note(char *task) {
  if (!task)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_task_mu);
  int id = pli_task_next++;
  *(int *)task = id;
  pli_task_entry_for(id);
  rt_lock_release(&pli_task_mu);
}

/* PRIORITY(t): read a task handle's priority as a value relative to the
 * current task (rule 79, ADR-298). */
int pli_task_priority_of(long long id) {
  if (id < 1)
    return 0;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  rt_lock_acquire(&pli_task_mu);
  pli_task_entry *e = pli_task_entry_for(id);
  int pri = e ? e->priority : 0;
  rt_lock_release(&pli_task_mu);
  /* Relative to the current task's priority. */
  return pri - rt_sched_current_priority();
}

/* PRIORITY(t) = n: set a task handle's priority so that a subsequent
 * PRIORITY(t) reads back n (rule 79, ADR-298). The stored value is
 * absolute: current + n. A live fiber whose handle still matches gets the
 * new value, so the change takes effect at the next scheduling decision. */
void pli_task_set_priority_of(long long id, int rel) {
  if (id < 1)
    return;
  pli_call_once(&pli_rt_once, pli_rt_init_sync);
  int abs = rel + rt_sched_current_priority();
  pli_fiber *live = NULL;
  rt_lock_acquire(&pli_task_mu);
  pli_task_entry *e = pli_task_entry_for(id);
  if (e) {
    e->priority = abs;
    live = e->fiber;
  }
  rt_lock_release(&pli_task_mu);
  /* A recycled fiber carries another handle; leave it alone. */
  if (live && atomic_load_explicit(&live->handle, memory_order_relaxed) == id)
    rt_sched_set_priority(live, abs);
}

