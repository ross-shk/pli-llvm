/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_sync.c — the portable runtime's lock layer (ADR-234).
 * The OS mode of rt_lock/rt_cond: the lock state lives in the platform
 * seam's mutex, so a task that blocks here blocks its OS thread — which
 * is the behaviour the runtime has today. The fiber mode (a holder that
 * parks its task) is added on top of this API later.
 */
#include "rt_sync.h"

void rt_lock_init(rt_lock *l) {
  pli_mutex_init(&l->os);
  l->inited = 1;
}

void rt_lock_init_recursive(rt_lock *l) {
  pli_mutex_init_recursive(&l->os);
  l->inited = 1;
}

void rt_cond_init(rt_cond *c) {
  pli_cond_init(&c->os);
  c->inited = 1;
}

void rt_lock_acquire(rt_lock *l) { pli_mutex_lock(&l->os); }

void rt_lock_release(rt_lock *l) { pli_mutex_unlock(&l->os); }

void rt_cond_wait(rt_cond *c, rt_lock *l) { pli_cond_wait(&c->os, &l->os); }

void rt_cond_broadcast(rt_cond *c) { pli_cond_broadcast(&c->os); }
