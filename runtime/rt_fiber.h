/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_fiber.h — the fiber control block and the context switch (ADR-234).
 *
 * A fiber is a stack plus the saved registers that switch it. The
 * scheduler (runtime/rt_sched.c) runs fibers; this header is the shape
 * the assembly in runtime/rt_fiber.S depends on.
 *
 * The first two members are the assembly's contract: offset 0 is the
 * saved stack pointer and offset 8 the resume address. Nothing may be
 * inserted before them.
 */
#ifndef PLI_RT_FIBER_H
#define PLI_RT_FIBER_H

#include <stddef.h>

typedef struct pli_fiber pli_fiber;

struct pli_fiber {
  void *sp;            /* offset 0: the saved stack pointer (the switch asm) */
  void *pc;            /* offset 8: the resume address (the switch asm)     */
  void *stack_base;    /* the usable low address of the reserved stack      */
  size_t stack_size;   /* the usable size of the reserved stack             */
  int state;           /* PLI_FIB_RUNNABLE / RUNNING / WAITING / DEAD        */
  pli_fiber *next;     /* the run-queue / wait-queue link                   */
  long long id;        /* the task id (the TASK variable's handle)          */
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
