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
 */
#include "rt_sync.h"
#include "rt_fiber.h"
#include "rt_sched.h"

void rt_lock_init(rt_lock *l) {
  pli_mutex_init(&l->os);
  l->owner = NULL;
  l->waiters = NULL;
  l->depth = 0;
  l->inited = 1;
}

void rt_lock_init_recursive(rt_lock *l) {
  pli_mutex_init_recursive(&l->os);
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
  pli_mutex_lock(&l->os);
  if (!l->owner) {
    l->owner = self;
    l->depth = 1;
    pli_mutex_unlock(&l->os);
    return;
  }
  if (l->owner == self) {
    l->depth++;
    pli_mutex_unlock(&l->os);
    return;
  }
  /* Contended: join the wait queue and park. The unlocker hands the lock
   * over directly, so there is nothing to re-check on waking. The owner is
   * recorded before the enqueue: the unlocker may run on another worker,
   * and only the owner may run this task again. */
  self->home = rt_sched_current_worker();
  self->wait_next = l->waiters;
  l->waiters = self;
  pli_mutex_unlock(&l->os);
  rt_sched_park();
}

void rt_lock_release(rt_lock *l) {
  if (!rt_sched_fiber_mode()) {
    pli_mutex_unlock(&l->os);
    return;
  }
  pli_mutex_lock(&l->os);
  if (l->depth > 1) {
    l->depth--;
    pli_mutex_unlock(&l->os);
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
    pli_mutex_unlock(&l->os);
    rt_sched_unpark(w);
    return;
  }
  pli_mutex_unlock(&l->os);
}

void rt_cond_wait(rt_cond *c, rt_lock *l) {
  if (!rt_sched_fiber_mode()) {
    pli_cond_wait(&c->os, &l->os);
    return;
  }
  pli_fiber *self = rt_sched_current();
  if (!self) {
    pli_cond_wait(&c->os, &l->os);
    return;
  }
  /* Enlist while the lock is held, then release it and park. A signaler
   * needs the lock, so it cannot slip in between the enlisting and the
   * release: no wakeup is lost. The wait queue is protected by the lock's
   * ownership, which the signaler holds while it broadcasts. The owner is
   * recorded before the enqueue: only the owner may run this task again. */
  self->home = rt_sched_current_worker();
  self->wait_next = c->waiters;
  c->waiters = self;
  rt_lock_release(l);
  rt_sched_park();
  rt_lock_acquire(l);
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
