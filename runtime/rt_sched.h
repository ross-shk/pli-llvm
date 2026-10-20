/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_sched.h — the user-space task scheduler (ADR-234).
 *
 * The scheduler runs the program's tasks as fibers: user-level execution
 * contexts that live on a small pool of OS worker threads. A task starts
 * and joins in tens of nanoseconds, and a program with a thousand tasks
 * uses a bounded number of OS threads.
 *
 * The legacy model — one OS thread per task — stays available through
 * PLI_TASK_MODEL=os and is what the runtime uses until the fiber model
 * becomes the default.
 */
#ifndef PLI_RT_SCHED_H
#define PLI_RT_SCHED_H

#include "rt_fiber.h"

/* Start the tasking runtime: pick the model and size the stacks. Call it
 * once, from pli_rt_init, before any task exists. */
void rt_sched_init(void);

/* Run `entry` as the program's main task. In the OS-thread model the
 * entry is called directly; in the fiber model it runs as a fiber, so the
 * scheduler can multiplex it with the program's tasks. */
void rt_sched_main(void (*entry)(void));

/* Start a task: fn(arg) runs as a fiber. Returns 0 on success. */
int rt_sched_spawn(void *(*fn)(void *), void *arg);

/* Park the current fiber until something unparks it. */
void rt_sched_park(void);

/* Park the current fiber until the monotonic deadline (in milliseconds
 * since an arbitrary origin). */
void rt_sched_park_until(long long deadline_ms);

/* Make a parked fiber runnable again. */
void rt_sched_unpark(pli_fiber *f);

/* The fiber running on this thread, or NULL outside a fiber. */
pli_fiber *rt_sched_current(void);

/* The worker running on this thread, or NULL outside a worker. A task that
 * joins a wait queue records it as the owner: only that worker may run the
 * task again (see the park/unpark contract in rt_sched.c). */
struct rt_worker *rt_sched_current_worker(void);

/* The per-task PL/I state of the running task: the ON-unit stack, ONCODE
 * and the CONTROLLED generation stacks. Under the fiber model it follows the
 * fiber; under the legacy model it is the thread's copy. */
pli_task_local *rt_task_local(void);

/* 1 when the fiber model is active, 0 for the legacy OS-thread model. */
int rt_sched_fiber_mode(void);

#endif /* PLI_RT_SCHED_H */
