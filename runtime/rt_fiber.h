/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fiber.h — the fiber control block and the context switch (ADR-234).
 *
 * A fiber is a stack plus the saved registers that switch it. The
 * scheduler (runtime/rt_sched.c) runs fibers; this header is the shape
 * the assembly in runtime/rt_fiber_asm.c depends on.
 *
 * The first two members are the assembly's contract: offset 0 is the
 * saved stack pointer and offset 8 the resume address. Nothing may be
 * inserted before them.
 */
#ifndef PLI_RT_FIBER_H
#define PLI_RT_FIBER_H

#include <stdatomic.h>
#include <stddef.h>

typedef struct pli_fiber pli_fiber;
struct rt_worker;

/* The per-task PL/I state (ADR-234).
 *
 * QR2.8 gives every task its own ON-unit stack and ONCODE, and the
 * CONTROLLED generation stacks belong to the task that allocated them.
 * Under the legacy model that state is thread-local, one copy per OS
 * thread; under the fiber model it must follow the *fiber*, because a
 * worker runs many tasks in turn. */
/* Initial capacity of a task's ON-unit stack. The stack lives on the heap and
 * doubles when it fills, so nesting has no language-level limit (C7). */
#define PLI_ON_INIT 16

/* One ON-unit stack entry (rules (91)-(94)). */
typedef struct {
  long long key; /* 0 = ERROR, else the sema-assigned condition key */
  long long src; /* the source condition key of the SIGNAL being handled */
  long long id;  /* handler id; 0 = the system action */
  void *fn;      /* compiled handler address; NULL = system action */
  void *ctx;     /* capture context; NULL when the unit touches no automatics */
} pli_on_frame;

typedef struct {
  pli_on_frame *on_stack; /* heap-grown ON-unit stack */
  int on_sp;              /* its depth */
  int on_cap;             /* its allocated capacity */
  int oncode;                        /* ONCODE */
  int on_source;                     /* the pending ONSOURCE key */
  void *ctl_vars;                    /* the CONTROLLED table (rt_storage.c) */
  void *ctl_last;                    /* one-entry lookup cache for ctl_vars */
  long long ctl_count;               /* its entry count */
  long long ctl_cache_key;           /* the CONTROLLED freelist shape */
  void *ctl_cache_bytes;             /* the CONTROLLED freelist buffer */
  long long ctl_cache_len;           /* its length */
  void *finish_fn;                   /* FINISH (rule 94, ADR-322): the task's
                                      * procedure, named by the spawn wrapper;
                                      * NULL outside a task */
} pli_task_local;

struct pli_fiber {
  void *sp;            /* offset 0: the saved stack pointer (the switch asm) */
  void *pc;            /* the resume address (informational)                */
  void *stack_base;    /* the usable low address of the reserved stack      */
  size_t stack_size;   /* the usable size of the reserved stack             */
  /* Atomic: a parked fiber is marked WAITING by its own worker and RUNNABLE
   * by whichever thread unparks it (ADR-325 wake-on-spawn), so the status
   * flag is written from two threads. It is informational only — the run
   * queue carries the hand-off — hence relaxed stores. */
  _Atomic int state;   /* PLI_FIB_RUNNABLE / RUNNING / WAITING / DEAD        */
  _Atomic int priority; /* the task's absolute priority (ADR-298)             */
  _Atomic long long handle; /* the TASK handle this fiber runs (ADR-298)     */
  _Atomic(pli_fiber *) next; /* the run-queue link: atomic because a thief
                              * reads it while the owner pops (both sides of
                              * the CAS), and only the CAS publishes it */
  pli_fiber *wait_next;/* the wait-queue link (a lock or a condition)       */
  const void *wait_key;/* what the task waits for (a condition's key)      */
  long long deadline;  /* the DELAY deadline (ms since an arbitrary origin) */
  void *(*entry_fn)(void *);   /* the task body, set by the scheduler          */
  void *entry_arg;             /* its argument                                */
  pli_task_local *local;       /* the per-task PL/I state, allocated on demand */
  struct rt_worker *home;      /* the worker that owns the suspended fiber   */
};

/* The fiber states. */
#define PLI_FIB_RUNNABLE 0
#define PLI_FIB_RUNNING 1
#define PLI_FIB_WAITING 2
#define PLI_FIB_DEAD 3

/* Switch from one fiber to another: saves the callee-saved registers,
 * the stack pointer and the resume address of `from`, restores those of
 * `to`, and jumps to `to`'s resume address. Symmetric: switching back to
 * `from` resumes it exactly where it left off. */
void pli_fiber_switch(pli_fiber *from, pli_fiber *to);

/* Lay up a fresh fiber on a stack whose high end is `stack_top`, so that
 * switching to it jumps to `entry` with the platform's entry stack
 * layout. The entry point must not take its arguments in registers: it
 * finds them through the scheduler's current-fiber pointer. */
void pli_fiber_start(pli_fiber *f, void *stack_top, void *entry);

#endif /* PLI_RT_FIBER_H */
