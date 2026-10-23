/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fbroker.c — file-read parking for the fiber model (C2 DEFERRED-WORK).
 *
 * A task that reads a FILE under the fiber model must not block the worker
 * it runs on. The read happens on a dedicated thread; the task parks until
 * the bytes arrive. This mirrors the stdin broker (ADR-238) but serves
 * arbitrary FILE streams.
 *
 * A GET statement holds the input lock (ADR-233) for its whole duration,
 * so only one task is inside the broker at a time and a single work slot
 * is enough.
 */
#include "pli_rt.h"
#include "rt_fiber.h"
#include "rt_sched.h"

/* The work slot: the reader thread picks up a request from here. There is
 * one slot because the compiler brackets a GET statement with the input
 * lock (ADR-233), so only one task is inside the broker at a time. */
struct rt_fbroker_req {
  FILE *f;
  void *buf;
  size_t n;
  long long result; /* filled by the reader thread; -1 = pending */
};

static struct {
  pli_mutex mu;
  pli_cond request_cv;  /* the reader waits here for a request */
  struct rt_fbroker_req req;
  struct pli_fiber *waiter; /* the task parked for the read */
  pli_thread th;
  pli_once once;
} rt_fbroker;

/* The reader thread: it does the blocking read so a task never does. */
static void *rt_fbroker_thread(void *arg) {
  (void)arg;
  for (;;) {
    pli_mutex_lock(&rt_fbroker.mu);
    while (!rt_fbroker.req.f)
      pli_cond_wait(&rt_fbroker.request_cv, &rt_fbroker.mu);
    struct rt_fbroker_req r = rt_fbroker.req;
    pli_mutex_unlock(&rt_fbroker.mu);

    /* The blocking read happens here, on the reader thread. */
    size_t got = pli_plat_fread(r.buf, 1, r.n, r.f);

    pli_mutex_lock(&rt_fbroker.mu);
    rt_fbroker.req.result = (long long)got;
    /* Clear f so the reader thread can pick up the next request. */
    rt_fbroker.req.f = NULL;
    struct pli_fiber *w = rt_fbroker.waiter;
    rt_fbroker.waiter = NULL;
    pli_mutex_unlock(&rt_fbroker.mu);
    /* Wake the task that parked for the read. */
    if (w)
      rt_sched_unpark(w);
  }
  return NULL;
}

static void rt_fbroker_start(void) {
  pli_mutex_init(&rt_fbroker.mu);
  pli_cond_init(&rt_fbroker.request_cv);
  rt_fbroker.req.f = NULL;
  rt_fbroker.req.result = -1;
  rt_fbroker.waiter = NULL;
  if (pli_spawn(&rt_fbroker.th, rt_fbroker_thread, NULL) != 0)
    pli_abort("TASK: could not start the file-read broker");
  pli_thread_detach(rt_fbroker.th);
}

/* Read `n` bytes from `f` into `buf`. Under the fiber model the task parks
 * while a helper thread does the blocking read; outside a fiber it reads
 * directly. Returns the byte count from fread (0 = EOF). */
size_t rt_fbroker_fread(void *buf, size_t n, FILE *f) {
  struct pli_fiber *self = rt_sched_current();
  if (!self || !rt_sched_fiber_mode())
    return pli_plat_fread(buf, 1, n, f);

  pli_call_once(&rt_fbroker.once, rt_fbroker_start);
  pli_mutex_lock(&rt_fbroker.mu);
  /* Record the owner before publishing the waiter: the reader may unpark
   * us before rt_sched_park runs, and an unpark hands the fiber to its
   * owner (mirrors rt_input.c). */
  self->home = rt_sched_current_worker();
  /* Publish the request and ourselves as the waiter. */
  rt_fbroker.req.f = f;
  rt_fbroker.req.buf = buf;
  rt_fbroker.req.n = n;
  rt_fbroker.req.result = -1;
  rt_fbroker.waiter = self;
  pli_cond_signal(&rt_fbroker.request_cv);
  /* Release the mutex before parking (mirrors rt_input.c). */
  pli_mutex_unlock(&rt_fbroker.mu);
  /* Park: the worker runs another fiber while the reader does the fread. */
  rt_sched_park();
  /* Resumed by the reader thread after the read completes. */
  pli_mutex_lock(&rt_fbroker.mu);
  long long got = rt_fbroker.req.result;
  pli_mutex_unlock(&rt_fbroker.mu);
  return got < 0 ? 0 : (size_t)got;
}
