/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_sync.c — the portable runtime's lock layer (ADR-234).
 *
 * The OS mode forwards to the platform seam, so the behaviour is identical
 * to the code this layer replaced. The fiber mode keeps the lock state in
 * the lock itself and parks the *task* when it cannot proceed, so a
 * blocked task does not block the worker it runs on.
 *
 * The fiber mode guards the lock's state (the owner, the recursion depth
 * and the wait queue) with the lock's own OS mutex, but never holds it
 * across a park: the mutex is taken for a few instructions, so the workers
 * never block each other for long. A contended lock hands the ownership
 * straight to the first waiter, so a woken task never has to re-check.
 *
 * The condition variable is *keyed*: a waiter says what it waits for, and
 * a signal wakes only the waiters that said so. The event table has one
 * condition for all the events, and a plain broadcast would wake every
 * waiter for every task that ends - the fan-out benchmark showed 19732
 * wakeups for 20000 tasks, nearly all of them useless.
 */
#include "rt_sync.h"
#include "rt_fiber.h"
#include "rt_sched.h"

/* The metadata lock: a test-and-set spin lock.
 *
 * It guards a handful of words (the owner, the depth, the wait queue) and
 * it is never held across a park, so the critical section is a handful of
 * instructions. An OS mutex would be far more expensive: its contended
 * path is a kernel wait, and the event table is taken by every task that
 * starts and by every task that ends.
 *
 * The lock is *not* held while a task waits for the lock itself: a task
 * that finds the lock taken joins the wait queue and parks, and the
 * unlocker hands the lock over to it directly. */
#if defined(__i386__) || defined(__x86_64__)
/* The pause hint: it tells the CPU the spin loop is hot, so it yields the
 * pipeline instead of burning it. */
#define RT_SPIN_PAUSE() __builtin_ia32_pause()
#else
#define RT_SPIN_PAUSE() ((void)0)
#endif

static void rt_meta_lock(rt_lock *l) {
  while (atomic_exchange_explicit(&l->meta, 1, memory_order_acquire))
    RT_SPIN_PAUSE(); /* spin: the critical section is a few instructions */
}

static void rt_meta_unlock(rt_lock *l) {
  atomic_store_explicit(&l->meta, 0, memory_order_release);
}

void rt_lock_init(rt_lock *l) {
  pli_mutex_init(&l->os);
  atomic_store(&l->meta, 0);
  l->owner = NULL;
  l->waiters = NULL;
  l->depth = 0;
  l->inited = 1;
}

void rt_lock_init_recursive(rt_lock *l) {
  pli_mutex_init_recursive(&l->os);
  atomic_store(&l->meta, 0);
  l->owner = NULL;
  l->waiters = NULL;
  l->depth = 0;
  l->inited = 1;
}

void rt_cond_init(rt_cond *c) {
  pli_cond_init(&c->os);
  c->waiters = NULL;
  c->inited = 1;
}

void rt_lock_acquire(rt_lock *l) {
  if (!rt_sched_fiber_mode()) {
    pli_mutex_lock(&l->os);
    return;
  }
  pli_fiber *self = rt_sched_current();
  if (!self) {
    /* Not in a fiber: only the legacy model reaches this, and there the
     * OS mutex is the lock. */
    pli_mutex_lock(&l->os);
    return;
  }
  rt_meta_lock(l);
  if (!l->owner) {
    l->owner = self;
    l->depth = 1;
    rt_meta_unlock(l);
    return;
  }
  if (l->owner == self) {
    l->depth++;
    rt_meta_unlock(l);
    return;
  }
  /* Contended: join the wait queue and park. The unlocker hands the lock
   * over directly, so there is nothing to re-check on waking. The owner is
   * recorded before the enqueue: the unlocker may run on another worker,
   * and only the owner may run this task again. */
  self->home = rt_sched_current_worker();
  self->wait_next = l->waiters;
  l->waiters = self;
  rt_meta_unlock(l);
  rt_sched_park();
}

void rt_lock_release(rt_lock *l) {
  if (!rt_sched_fiber_mode()) {
    pli_mutex_unlock(&l->os);
    return;
  }
  rt_meta_lock(l);
  if (l->depth > 1) {
    l->depth--;
    rt_meta_unlock(l);
    return;
  }
  l->depth = 0;
  l->owner = NULL;
  /* Hand the lock to the first waiter, if any. */
  pli_fiber *w = l->waiters;
  if (w) {
    l->waiters = w->wait_next;
    w->wait_next = NULL;
    l->owner = w;
    l->depth = 1;
    rt_meta_unlock(l);
    rt_sched_unpark(w);
    return;
  }
  rt_meta_unlock(l);
}

/* Enlist the current task on the condition's wait queue, then release the
 * lock and park. The key says what the task waits for; a NULL key means
 * "anything", which is what a waiter that watches a whole set uses. */
static void rt_cond_wait_key(rt_cond *c, rt_lock *l, const void *key) {
  pli_fiber *self = rt_sched_current();
  if (!self) {
    pli_cond_wait(&c->os, &l->os);
    return;
  }
  /* Enlist while the lock is held, then release it and park. A signaler
   * needs the lock, so it cannot slip in between the enlisting and the
   * release: no wakeup is lost. The wait queue is protected by the lock's
   * ownership, which the signaler holds while it signals. The owner is
   * recorded before the enqueue: only the owner may run this task again. */
  self->home = rt_sched_current_worker();
  self->wait_key = key;
  self->wait_next = c->waiters;
  c->waiters = self;
  rt_lock_release(l);
  rt_sched_park();
  rt_lock_acquire(l);
}

void rt_cond_wait(rt_cond *c, rt_lock *l) {
  if (!rt_sched_fiber_mode()) {
    pli_cond_wait(&c->os, &l->os);
    return;
  }
  rt_cond_wait_key(c, l, NULL);
}

void rt_cond_wait_keyed(rt_cond *c, rt_lock *l, const void *key) {
  if (!rt_sched_fiber_mode()) {
    pli_cond_wait(&c->os, &l->os);
    return;
  }
  rt_cond_wait_key(c, l, key);
}

/* Walk the wait queue and wake the waiters whose key matches. A waiter
 * with a NULL key waits for anything, so it is woken too. */
void rt_cond_signal_key(rt_cond *c, const void *key) {
  if (!rt_sched_fiber_mode()) {
    pli_cond_broadcast(&c->os);
    return;
  }
  pli_fiber *w = c->waiters;
  pli_fiber *keep = NULL;
  pli_fiber *keep_tail = NULL;
  while (w) {
    pli_fiber *n = w->wait_next;
    w->wait_next = NULL;
    if (!w->wait_key || w->wait_key == key) {
      rt_sched_unpark(w);
    } else {
      /* Not for us: put it back. */
      if (keep_tail)
        keep_tail->wait_next = w;
      else
        keep = w;
      keep_tail = w;
    }
    w = n;
  }
  c->waiters = keep;
}

void rt_cond_broadcast(rt_cond *c) {
  if (!rt_sched_fiber_mode()) {
    pli_cond_broadcast(&c->os);
    return;
  }
  pli_fiber *w = c->waiters;
  c->waiters = NULL;
  while (w) {
    pli_fiber *n = w->wait_next;
    w->wait_next = NULL;
    rt_sched_unpark(w);
    w = n;
  }
}
