/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_sync.h — the portable runtime's lock layer (ADR-234).
 *
 * One API for the whole runtime: the tasking code (rt_task.c) and the
 * stream serialiser (rt_stream.c) lock through rt_lock/rt_cond instead of
 * naming the platform seam's pli_mutex/pli_cond directly. The seam stays
 * the host's; this layer is what the fiber scheduler sits on.
 *
 * Two modes, fixed once at pli_rt_init before any lock is taken, so a
 * lock object never sees both:
 *   OS    — the legacy path: the lock is the platform's mutex and a
 *           contended wait blocks the OS thread;
 *   FIBER — the lock is a state word plus a wait queue, and a contended
 *           wait parks the *task*, so the worker runs other tasks.
 */
#ifndef PLI_RT_SYNC_H
#define PLI_RT_SYNC_H

#include "pli_rt_plat.h"
#include <stdatomic.h>

/* The fiber is opaque here: this layer only holds pointers to fibers. */
struct pli_fiber;

/* A lock. Two flavours, matching the two uses in the runtime:
 *   plain      — the EVENT/task tables; nobody takes them twice;
 *   recursive  — the stream serialiser, whose item writer calls the byte
 *                writer, which locks on its own (ADR-233).
 * The distinction is kept because a recursive mutex held twice across a
 * condition wait would not be fully released, which can deadlock. */
typedef struct {
  pli_mutex os;               /* the OS mode's mutex */
  _Atomic int meta;           /* the FIBER mode's metadata lock */
  struct pli_fiber *owner;    /* the FIBER mode's holder */
  struct pli_fiber *waiters;  /* the FIBER mode's wait queue */
  long depth;                 /* the recursion depth */
} rt_lock;

/* A condition variable, paired with an rt_lock. */
typedef struct {
  pli_cond os;                /* the OS mode's condvar */
  struct pli_fiber *waiters;  /* the FIBER mode's wait queue */
} rt_cond;

/* Initialise a lock / a condition. Not thread-safe by itself: call it
 * before the runtime hands the lock to another thread (the one-time init
 * does exactly that). */
void rt_lock_init(rt_lock *l);
void rt_lock_init_recursive(rt_lock *l);
void rt_cond_init(rt_cond *c);

/* Take / release a lock. Recursive locks may be taken again by their
 * holder. (The operations are named acquire/release: `rt_lock` is the
 * type, and a typedef shares C's ordinary identifier namespace with
 * functions.) */
void rt_lock_acquire(rt_lock *l);
void rt_lock_release(rt_lock *l);

/* Wait on a condition until it is broadcast, then re-take the lock. The
 * caller must hold the lock; on return the caller holds it again. */
void rt_cond_wait(rt_cond *c, rt_lock *l);

/* Wait for one specific thing.
 *
 * A plain condition variable wakes every waiter, so a program with many
 * waiters on one condition pays for the wakeups it did not need: the
 * event table has one condition for all the events, and a task that ends
 * would wake every task that waits for another event. A keyed wait names
 * what the waiter waits for, and a keyed signal wakes only the waiters
 * that named it.
 *
 * `key` is the thing waited for; a NULL key means "anything", which is
 * what a waiter that watches a whole set of events uses. */
void rt_cond_wait_keyed(rt_cond *c, rt_lock *l, const void *key);

/* Wake every waiter of a condition. The caller must hold the lock. */
void rt_cond_broadcast(rt_cond *c);

/* Wake the waiters that wait for `key` (and the waiters that wait for
 * anything). The caller must hold the lock. */
void rt_cond_signal_key(rt_cond *c, const void *key);

#endif /* PLI_RT_SYNC_H */
