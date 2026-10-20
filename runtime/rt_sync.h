/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_sync.h — the portable runtime's lock layer (ADR-234).
 *
 * One API for the whole runtime: the tasking code (rt_task.c) and the
 * stream serialiser (rt_stream.c) lock through rt_lock/rt_cond instead of
 * naming the platform seam's pli_mutex/pli_cond directly. The seam stays
 * the host's; this layer is what the fiber scheduler will sit on.
 *
 * Why a layer at all: under the fiber model a lock holder parks its *task*
 * rather than its OS thread, so the lock must know about fibers. Keeping
 * that knowledge here means the tasking and stream code never mentions a
 * fiber, and the port to the fiber model touches this file only.
 *
 * This phase forwards to the OS primitives, so the behaviour is identical
 * to the code it replaces. The fiber mode (a holder that parks its task)
 * is added later without changing this API.
 */
#ifndef PLI_RT_SYNC_H
#define PLI_RT_SYNC_H

#include "pli_rt_plat.h"

/* A lock. Two flavours, matching the two uses in the runtime:
 *   plain      — the EVENT/task tables; nobody takes them twice;
 *   recursive  — the stream serialiser, whose item writer calls the byte
 *                writer, which locks on its own (ADR-233).
 * The distinction is kept because a recursive mutex held twice across a
 * condition wait would not be fully released, which can deadlock. */
typedef struct {
  pli_mutex os; /* the OS mutex behind the lock */
  int inited;
} rt_lock;

/* A condition variable, paired with an rt_lock. */
typedef struct {
  pli_cond os; /* the OS condvar behind the condition */
  int inited;
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

/* Wake every waiter of a condition. The caller must hold the lock. */
void rt_cond_broadcast(rt_cond *c);

#endif /* PLI_RT_SYNC_H */
