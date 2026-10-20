/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_sched.c — the user-space task scheduler (ADR-234).
 *
 * M user-level tasks on N OS threads (the workers). Each worker owns a
 * local LIFO run queue; a task pushed on another worker's queue is stolen
 * half-at-a-time, Go-style. A worker with nothing to run parks on the
 * scheduler's condvar — an OS block is fine there, because every task on
 * that worker is blocked too.
 *
 * The queues are the hot path, so they are lock-free: a worker's queue head
 * is an atomic pointer, and a push publishes the link with a compare-and-swap
 * (a stealer may detach the list at any moment). The scheduler's OS lock
 * guards only what the workers share: the global queue (the tasks pushed by
 * a thread that is not a worker), the DELAY timer heap, and the parked-worker
 * count.
 */
#include "pli_rt.h"
#include "rt_fiber.h"
#include "rt_sched.h"
#include <stdatomic.h>

/* The model: 1 = the fiber model, 0 = the legacy OS-thread model. The
 * choice is made once, at pli_rt_init, before any task exists, so a lock
 * object never sees both. */
static int rt_model_fiber = 0;

/* The fiber backend exists on x86-64 (the SysV and the Win64 assembly in
 * runtime/rt_fiber_asm.c). Elsewhere the runtime keeps the legacy model. */
#if defined(__x86_64__)
#define RT_HAVE_FIBER 1
#else
#define RT_HAVE_FIBER 0
#endif

/* The fiber running on this thread. */
static _Thread_local pli_fiber *rt_cur = NULL;

/* The worker running on this thread (NULL outside a worker). */
static _Thread_local struct rt_worker *rt_self = NULL;

/* The per-task PL/I state outside a fiber: the legacy model's copy, one
 * per OS thread (the behaviour the fiber model replaces). */
static _Thread_local pli_task_local rt_tls_local;

/* The current fiber and the current worker are read through noinline
 * accessors, never directly.
 *
 * A fiber may migrate: a worker that finds nothing to run steals a task
 * from a peer, and the task resumes on the *other* worker. The compiler
 * would otherwise keep the thread's TLS address in a callee-saved register
 * across the switch, and the resumed code would then read the previous
 * worker's thread-local state — the previous worker's TLS, once that thread
 * exits, is gone. Reading the TLS inside a function that the compiler
 * cannot inline forces a fresh read on every use, on the current thread. */
#if defined(_MSC_VER)
#define PLI_NOINLINE __declspec(noinline)
#else
#define PLI_NOINLINE __attribute__((noinline))
#endif

/* The workers. Worker 0 is the thread that starts the program. */
#define RT_WORKERS_MAX 64

typedef struct rt_worker {
  _Atomic(pli_fiber *) local; /* the worker's LIFO run queue */
  _Atomic(pli_fiber *) ready; /* the fibers unparked while they slept here */
  pli_fiber *cur;             /* the fiber running on this worker */
  pli_fiber *sched_ctx;       /* the worker's own context (its OS stack) */
  _Atomic int parked;         /* 1 while the worker waits on the condvar */
  int id;
} rt_worker;

static rt_worker rt_workers[RT_WORKERS_MAX];
static int rt_worker_count = 1;
static int rt_workers_started = 0;

/* The noinline accessors (see the note above the thread-locals). */
PLI_NOINLINE static pli_fiber *rt_cur_get(void) { return rt_cur; }
PLI_NOINLINE static rt_worker *rt_self_get(void) { return rt_self; }

/* The program's main task: the fiber the MAIN procedure runs on. */
static pli_fiber *rt_main_fib = NULL;
static _Atomic int rt_main_done = 0;

/* The scheduler's lock: the workers are OS threads, so the state they share
 * is guarded by an OS lock. The fiber-mode lock in rt_sync.c guards *tasks*,
 * not the workers. */
static pli_mutex rt_sched_mu;
static pli_cond rt_sched_cv;

/* The global queue: the tasks pushed by a thread that is not a worker. */
static pli_fiber *rt_global_head = NULL;

/* The timers: the fibers parked until a deadline (DELAY), a min-heap. */
static pli_fiber **rt_timer_heap = NULL;
static int rt_timer_count = 0;
static int rt_timer_cap = 0;

/* The stack ceiling: PLI_TASK_STACK bytes. The pages are demand-paged, so
 * the resident cost is only what a task actually touches. */
#define RT_STACK_DEFAULT (8u * 1024u * 1024u)
#define RT_STACK_MIN (64u * 1024u)
static size_t rt_stack_bytes = RT_STACK_DEFAULT;

/* Parse a non-negative decimal from a string. The portable blob may not
 * name libc's strtoll (it must call down to the platform TU), so the
 * parser is local. */
static long long rt_parse_ll(const char *s) {
  long long v = 0;
  if (!s)
    return 0;
  while (*s == ' ' || *s == '\t')
    ++s;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    ++s;
  }
  return v;
}

void rt_sched_init(void) {
  /* The fiber model is the default (ADR-234). PLI_TASK_MODEL=os selects
   * the legacy OS-thread model (ADR-016) for A/B runs and for the
   * platforms that have no fiber backend yet. */
  const char *model = pli_plat_getenv("PLI_TASK_MODEL");
  int want_fiber = RT_HAVE_FIBER;
  if (model && model[0] && (model[0] == 'o' || model[0] == 'O'))
    want_fiber = 0;
  rt_model_fiber = want_fiber ? 1 : 0;

  long long n = rt_parse_ll(pli_plat_getenv("PLI_TASK_STACK"));
  if (n >= (long long)RT_STACK_MIN)
    rt_stack_bytes = (size_t)n;

  /* The worker count: PLI_TASK_WORKERS, else min(cores, 8). */
  long long w = rt_parse_ll(pli_plat_getenv("PLI_TASK_WORKERS"));
  if (w > 0) {
    rt_worker_count = (int)(w > RT_WORKERS_MAX ? RT_WORKERS_MAX : w);
  } else {
    int cores = pli_plat_cpu_count();
    if (cores < 1)
      cores = 1;
    rt_worker_count = cores > 8 ? 8 : cores;
  }
  if (rt_worker_count < 1)
    rt_worker_count = 1;

  if (rt_model_fiber) {
    pli_mutex_init(&rt_sched_mu);
    pli_cond_init(&rt_sched_cv);
    rt_workers[0].id = 0;
    rt_self = &rt_workers[0];
  }
}

int rt_sched_fiber_mode(void) { return rt_model_fiber; }

pli_fiber *rt_sched_current(void) { return rt_cur_get(); }

struct rt_worker *rt_sched_current_worker(void) { return rt_self_get(); }

/* The per-task PL/I state of the running task (QR2.8). Under the fiber model
 * it belongs to the fiber, so a worker that runs a thousand tasks in turn
 * gives each of them its own ON stack and CONTROLLED generations; the copy
 * is allocated on first use, so a task that touches neither pays nothing. */
pli_task_local *rt_task_local(void) {
  if (rt_model_fiber) {
    pli_fiber *f = rt_cur_get();
    if (f) {
      if (!f->local) {
        f->local = (pli_task_local *)pli_plat_calloc(1, sizeof(pli_task_local));
        if (!f->local)
          pli_abort("TASK: out of memory");
      }
      return f->local;
    }
  }
  return &rt_tls_local;
}

/* --- the lock-free run queues ---
 *
 * A queue is a singly linked LIFO: the head is the newest task, the tail
 * the oldest. The head is atomic because a stealer detaches the whole list
 * at any moment; the links are written only by the owner before the head is
 * published, and read by a thief only after it detached the list. */

/* Push one task. The link is published with a compare-and-swap, so a
 * concurrent steal cannot lose the task. */
static void rt_queue_push(_Atomic(pli_fiber *) *head, pli_fiber *f) {
  pli_fiber *h;
  do {
    h = atomic_load_explicit(head, memory_order_relaxed);
    f->next = h;
  } while (!atomic_compare_exchange_weak_explicit(head, &h, f,
                                                  memory_order_release,
                                                  memory_order_relaxed));
}

/* Pop the newest task (the owner's own queue). */
static pli_fiber *rt_queue_pop(_Atomic(pli_fiber *) *head) {
  pli_fiber *h, *next;
  do {
    h = atomic_load_explicit(head, memory_order_acquire);
    if (!h)
      return NULL;
    next = h->next;
  } while (!atomic_compare_exchange_weak_explicit(head, &h, next,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));
  h->next = NULL;
  return h;
}

/* The tail (the oldest task) of a list. */
static pli_fiber *rt_list_tail(pli_fiber *p) {
  while (p->next)
    p = p->next;
  return p;
}

/* Push a whole list as one block: the list's head is the newest task. */
static void rt_queue_push_list(_Atomic(pli_fiber *) *head, pli_fiber *list) {
  pli_fiber *tail = rt_list_tail(list);
  pli_fiber *h;
  do {
    h = atomic_load_explicit(head, memory_order_acquire);
    tail->next = h;
  } while (!atomic_compare_exchange_weak_explicit(head, &h, list,
                                                  memory_order_release,
                                                  memory_order_relaxed));
}

/* Steal half of a peer's queue: detach its list, keep the newer half and
 * put the older half back, so the victim keeps a share of the work. */
static pli_fiber *rt_queue_steal(_Atomic(pli_fiber *) *head) {
  pli_fiber *list = atomic_exchange_explicit(head, NULL, memory_order_acq_rel);
  if (!list)
    return NULL;
  int n = 0;
  for (pli_fiber *p = list; p; p = p->next)
    ++n;
  if (n < 2)
    return list;
  int half = n / 2;
  pli_fiber *split = list;
  for (int i = 1; i < half; ++i)
    split = split->next;
  pli_fiber *rest = split->next; /* the older half */
  split->next = NULL;
  /* Put the older half back in front of whatever the victim pushed since. */
  pli_fiber *tail = rt_list_tail(rest);
  pli_fiber *h;
  do {
    h = atomic_load_explicit(head, memory_order_acquire);
    tail->next = h;
  } while (!atomic_compare_exchange_weak_explicit(head, &h, rest,
                                                  memory_order_release,
                                                  memory_order_acquire));
  return list; /* the newer half: the thief's */
}

/* --- the global queue and the timers (the scheduler's OS lock) --- */

static void rt_wake_one(rt_worker *w);

/* The tasks pushed by a thread that is not a worker. */
static pli_fiber *rt_take_global(void) {
  pli_mutex_lock(&rt_sched_mu);
  pli_fiber *f = rt_global_head;
  if (f)
    rt_global_head = f->next;
  pli_mutex_unlock(&rt_sched_mu);
  if (f)
    f->next = NULL;
  return f;
}

static void rt_push_global(pli_fiber *f) {
  pli_mutex_lock(&rt_sched_mu);
  f->next = rt_global_head;
  rt_global_head = f;
  pli_mutex_unlock(&rt_sched_mu);
}

/* The timer heap: the fibers parked until a deadline, ordered by it. */
static void rt_timer_push(pli_fiber *f) {
  pli_mutex_lock(&rt_sched_mu);
  if (rt_timer_count == rt_timer_cap) {
    int cap = rt_timer_cap ? rt_timer_cap * 2 : 16;
    pli_fiber **h = (pli_fiber **)pli_plat_realloc(rt_timer_heap,
                                                    (size_t)cap * sizeof(pli_fiber *));
    if (!h) {
      pli_mutex_unlock(&rt_sched_mu);
      pli_abort("TASK: out of memory");
    }
    rt_timer_heap = h;
    rt_timer_cap = cap;
  }
  int i = rt_timer_count++;
  while (i > 0) {
    int parent = (i - 1) / 2;
    if (rt_timer_heap[parent]->deadline <= f->deadline)
      break;
    rt_timer_heap[i] = rt_timer_heap[parent];
    i = parent;
  }
  rt_timer_heap[i] = f;
  pli_mutex_unlock(&rt_sched_mu);
}

/* The earliest deadline in the heap, or -1 when the heap is empty. */
static long long rt_timer_next_deadline(void) {
  pli_mutex_lock(&rt_sched_mu);
  long long d = rt_timer_count ? rt_timer_heap[0]->deadline : -1;
  pli_mutex_unlock(&rt_sched_mu);
  return d;
}

/* Take the earliest timer that has matured. The fiber goes to its owner's
 * ready list, not straight back into a run queue: a timer can mature while
 * the fiber is still parking, and only the owner may run a fiber that is
 * in the middle of suspending itself. */
static pli_fiber *rt_take_expired_timer(void) {
  long long now = pli_plat_now_ms();
  pli_mutex_lock(&rt_sched_mu);
  pli_fiber *f = NULL;
  if (rt_timer_count && rt_timer_heap[0]->deadline <= now) {
    f = rt_timer_heap[0];
    int last = --rt_timer_count;
    if (last > 0) {
      pli_fiber *moved = rt_timer_heap[last];
      int i = 0;
      for (;;) {
        int l = 2 * i + 1, r = l + 1, best = i;
        if (l < last && rt_timer_heap[l]->deadline < moved->deadline)
          best = l;
        if (r < last && rt_timer_heap[r]->deadline < moved->deadline)
          best = r;
        if (best == i)
          break;
        pli_fiber *tmp = rt_timer_heap[i];
        rt_timer_heap[i] = rt_timer_heap[best];
        rt_timer_heap[best] = tmp;
        i = best;
      }
      rt_timer_heap[i] = moved;
    }
  }
  pli_mutex_unlock(&rt_sched_mu);
  if (!f)
    return NULL;
  f->next = NULL;
  f->state = PLI_FIB_RUNNABLE;
  rt_worker *home = f->home;
  if (home) {
    rt_queue_push(&home->ready, f);
    rt_wake_one(home);
  } else {
    rt_push_global(f);
    rt_wake_one(NULL);
  }
  return NULL;
}

/* --- the workers --- */

/* The next runnable task: the worker's own queue, then the global queue,
 * then a steal from a peer, then a matured DELAY. */
static pli_fiber *rt_next_runnable(rt_worker *w) {
  pli_fiber *f = rt_queue_pop(&w->local);
  if (f)
    return f;
  f = rt_take_global();
  if (f)
    return f;
  for (int i = 1; i < rt_worker_count; ++i) {
    rt_worker *victim = &rt_workers[(w->id + i) % rt_worker_count];
    pli_fiber *stolen = rt_queue_steal(&victim->local);
    if (stolen) {
      rt_queue_push_list(&w->local, stolen);
      return rt_queue_pop(&w->local);
    }
  }
  return rt_take_expired_timer();
}

/* Wake a parked worker, if there is one. The lock is taken only when the
 * worker is actually idle, so a busy scheduler never pays for it — and the
 * check is per worker: a task that unparks a task on a busy worker must not
 * wake the whole pool. */
static void rt_wake_one(rt_worker *w) {
  if (!w || !atomic_load_explicit(&w->parked, memory_order_relaxed))
    return;
  pli_mutex_lock(&rt_sched_mu);
  pli_cond_broadcast(&rt_sched_cv);
  pli_mutex_unlock(&rt_sched_mu);
}

/* Nothing to run: park on the scheduler's condvar until a task arrives or
 * the earliest DELAY matures. */
static void rt_worker_park(rt_worker *w) {
  long long deadline = rt_timer_next_deadline();
  long long timeout = -1; /* wait forever */
  if (deadline >= 0) {
    long long now = pli_plat_now_ms();
    timeout = deadline - now;
    if (timeout < 0)
      timeout = 0;
  }
  pli_mutex_lock(&rt_sched_mu);
  /* Re-check under the lock: a task may have arrived while we waited for it. */
  if (!atomic_load_explicit(&w->local, memory_order_acquire) && !rt_global_head) {
    atomic_store_explicit(&w->parked, 1, memory_order_release);
    pli_cond_timedwait(&rt_sched_cv, &rt_sched_mu, timeout);
    atomic_store_explicit(&w->parked, 0, memory_order_release);
  }
  pli_mutex_unlock(&rt_sched_mu);
}

/* The fiber trampoline. The assembly jumps to it, so it takes no register
 * arguments: it finds its work through the current fiber. */
static void rt_fiber_trampoline(void) {
  pli_fiber *f = rt_cur_get();

  void *(*fn)(void *) = f->entry_fn;
  void *arg = f->entry_arg;
  fn(arg);
  /* The task body returned. The compiled wrapper completes the event and
   * frees the context before it returns, so this is only reached when a
   * body skipped that — mark the fiber dead and hand the worker back. */
  f->state = PLI_FIB_DEAD;
  pli_fiber_switch(f, rt_self_get()->sched_ctx);
}

/* Lay up a fiber on a fresh or recycled stack. */
static int rt_fiber_prepare(pli_fiber *f, void *(*fn)(void *), void *arg) {
  f->entry_fn = fn;
  f->entry_arg = arg;
  f->state = PLI_FIB_RUNNABLE;
  f->next = NULL;
  f->wait_next = NULL;
  f->deadline = 0;
  /* A recycled fiber carries the previous task's state: the new task starts
   * with an empty ON stack (QR2.8) and no CONTROLLED generations. The
   * CONTROLLED table and the one-slot freelist are kept for reuse: the
   * table's entries are overwritten, and the cached buffer is a free one. */
  if (f->local) {
    f->local->on_sp = 0;
    f->local->oncode = 0;
    f->local->on_source = 0;
    f->local->ctl_count = 0;
  }
  if (!f->stack_base) {
    void *base = NULL;
    if (pli_plat_stack_alloc(rt_stack_bytes, &base) != 0)
      return -1;
    f->stack_base = base;
    f->stack_size = rt_stack_bytes;
  }
  pli_fiber_start(f, (char *)f->stack_base + f->stack_size, (void *)rt_fiber_trampoline);
  return 0;
}

/* The fiber pool: a global, lock-free stack. A fiber carries its stack, so
 * recycling a fiber recycles its stack too — that is the point of the pool:
 * a program that starts a million tasks allocates a handful of stacks. It
 * is global, not per worker, because a task may finish on a different
 * worker than the one that started it, and the stack must go back to the
 * pool that the next spawn looks in. */
static _Atomic(pli_fiber *) rt_free_fibers = NULL;

static pli_fiber *rt_fiber_get(void) {
  pli_fiber *f = atomic_load_explicit(&rt_free_fibers, memory_order_acquire);
  while (f) {
    pli_fiber *next = f->next;
    if (atomic_compare_exchange_weak_explicit(&rt_free_fibers, &f, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire))
      return f;
  }
  f = (pli_fiber *)pli_plat_malloc(sizeof(pli_fiber));
  if (!f)
    return NULL;
  f->stack_base = NULL;
  f->stack_size = 0;
  f->local = NULL;
  f->home = NULL;
  return f;
}

static void rt_fiber_put(pli_fiber *f) {
  pli_fiber *h;
  do {
    h = atomic_load_explicit(&rt_free_fibers, memory_order_relaxed);
    f->next = h;
  } while (!atomic_compare_exchange_weak_explicit(&rt_free_fibers, &h, f,
                                                  memory_order_release,
                                                  memory_order_relaxed));
}

/* Park and unpark.
 *
 * A parked fiber belongs to the worker that parked it: only that worker
 * may run it again. The reason is the window between "the fiber decided to
 * sleep" and "the fiber's registers are saved": another worker that ran
 * the fiber in that window would restore a half-saved context. So an
 * unpark hands the fiber to its owner's ready list, and the owner — the
 * worker that was in the middle of parking it — picks it up when it comes
 * back from the switch. */
void rt_sched_park(void) {
  pli_fiber *self = rt_cur_get();
  rt_worker *w = rt_self_get();
  if (!self || !w || !w->sched_ctx)
    return;
  self->state = PLI_FIB_WAITING;
  self->home = w;
  pli_fiber_switch(self, w->sched_ctx);
  /* Resumed: the scheduler made us runnable again. The fiber may have
   * moved to another worker, so nothing cached before the switch may be
   * used here. */
}

void rt_sched_unpark(pli_fiber *f) {
  if (!f)
    return;
  f->state = PLI_FIB_RUNNABLE;
  rt_worker *home = f->home;
  if (home) {
    rt_queue_push(&home->ready, f);
    rt_wake_one(home);
  } else {
    rt_push_global(f);
    rt_wake_one(NULL);
  }
}

void rt_sched_park_until(long long deadline_ms) {
  pli_fiber *self = rt_cur_get();
  rt_worker *w = rt_self_get();
  if (!self || !w || !w->sched_ctx)
    return;
  self->deadline = deadline_ms;
  self->state = PLI_FIB_WAITING;
  self->home = w;
  rt_timer_push(self);
  pli_fiber_switch(self, w->sched_ctx);
}

/* The worker loop: run a task until it yields, parks or finishes. */
static void rt_worker_loop(rt_worker *w) {
  /* The worker's own context: a parked task switches back to it, so it
   * lives in the worker's frame for as long as the worker runs. */
  pli_fiber sched_ctx;
  sched_ctx.sp = NULL;
  sched_ctx.pc = NULL;
  sched_ctx.state = PLI_FIB_RUNNING;
  w->sched_ctx = &sched_ctx;

  for (;;) {
    /* The program's main task is over: this worker has nothing to do. */
    if (w->id == 0 && atomic_load_explicit(&rt_main_done, memory_order_acquire))
      return;
    /* The fibers that were unparked while they slept on this worker. */
    pli_fiber *ready = atomic_exchange_explicit(&w->ready, NULL, memory_order_acq_rel);
    while (ready) {
      pli_fiber *n = ready->next;
      rt_queue_push(&w->local, ready);
      ready = n;
    }
    pli_fiber *f = rt_next_runnable(w);
    if (!f) {
      rt_worker_park(w);
      continue;
    }
    f->state = PLI_FIB_RUNNING;
    rt_cur = f;
    pli_fiber_switch(w->sched_ctx, f);
    /* Resumed: the task yielded, parked or finished. The worker is not a
     * task, so its per-task state is the thread's own again. */
    rt_cur = NULL;
    if (f->state == PLI_FIB_DEAD) {
      if (f == rt_main_fib) {
        /* The MAIN procedure is done: the program ends with it. */
        atomic_store_explicit(&rt_main_done, 1, memory_order_release);
        pli_mutex_lock(&rt_sched_mu);
        pli_cond_broadcast(&rt_sched_cv);
        pli_mutex_unlock(&rt_sched_mu);
        return;
      }
      rt_fiber_put(f);
    }
  }
}

/* A worker thread: the entry point of every worker but the first. */
static void *rt_worker_entry(void *arg) {
  rt_worker *w = (rt_worker *)arg;
  rt_self = w;
  rt_worker_loop(w);
  return NULL;
}

/* Start the worker threads. Worker 0 is the thread that starts the program,
 * so the others are the only ones created here; they are created when the
 * first task appears, so a program without tasks never pays for them. */
static void rt_workers_start(void) {
  if (rt_workers_started)
    return;
  rt_workers_started = 1;
  for (int i = 1; i < rt_worker_count; ++i) {
    rt_workers[i].id = i;
    pli_thread th;
    if (pli_spawn(&th, rt_worker_entry, &rt_workers[i]) != 0)
      break;
    pli_thread_detach(th);
  }
}

/* The main program's entry takes no argument, but the trampoline calls the
 * task body with one. */
static void *rt_main_entry(void *arg) {
  ((void (*)(void))arg)();
  return NULL;
}

void rt_sched_main(void (*entry)(void)) {
  if (!rt_model_fiber) {
    entry();
    return;
  }
  /* The main program runs as a fiber, so the scheduler can multiplex it
   * with the program's tasks: a WAIT in the main program parks it and the
   * worker runs the tasks instead. */
  rt_worker *w = &rt_workers[0];
  rt_self = w;

  pli_fiber main_fib;
  main_fib.sp = NULL;
  main_fib.pc = NULL;
  main_fib.stack_base = NULL;
  main_fib.stack_size = 0;
  main_fib.state = PLI_FIB_RUNNABLE;
  main_fib.next = NULL;
  main_fib.wait_next = NULL;
  main_fib.deadline = 0;
  main_fib.id = 0;
  main_fib.entry_fn = rt_main_entry;
  main_fib.entry_arg = (void *)entry;
  main_fib.local = NULL;
  if (rt_fiber_prepare(&main_fib, rt_main_entry, (void *)entry) != 0) {
    entry();
    return;
  }
  rt_main_fib = &main_fib;
  /* The main program is the first task the worker runs. */
  rt_queue_push(&w->local, &main_fib);
  rt_worker_loop(w);
}

int rt_sched_spawn(void *(*fn)(void *), void *arg) {
  if (!rt_model_fiber)
    return -1;
  rt_worker *w = rt_self_get();
  if (!w)
    return -1;
  rt_workers_start();
  pli_fiber *f = rt_fiber_get();
  if (!f)
    return -1;
  if (rt_fiber_prepare(f, fn, arg) != 0) {
    rt_fiber_put(f);
    return -1;
  }
  rt_queue_push(&w->local, f);
  rt_wake_one(w);
  return 0;
}

/* The PL/I-facing entry (the ABI name): run the MAIN procedure as a task.
 * The function pointer arrives as a char *, the same way pli_task_spawn
 * takes its callee — the cast goes straight from the object pointer to the
 * function pointer, since going through void * is not a conforming
 * conversion. */
void pli_sched_main(char *fn) { rt_sched_main((void (*)(void))fn); }

/* The fallback for a platform without a fiber backend: the assembly jumps
 * here instead of defining the switch. */
void rt_fiber_unsupported(void) { pli_abort("TASK: this platform has no fiber backend"); }
