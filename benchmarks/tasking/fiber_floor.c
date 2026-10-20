/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* fiber_floor.c — the ADR-234 floor measurement.
 *
 * The question this answers: what is the cheapest possible start+join for
 * a PL/I task? The plan's target is <=150 ns; Go's goroutine spawn plus
 * WaitGroup.Done is ~210-260 ns. This measures the raw cost of laying up
 * a fiber and switching to it and back, with no scheduler, no locking and
 * no allocation on the measured path (the stack is allocated once and
 * reused) - the floor the real scheduler starts from.
 *
 * Build (from the repo root):
 *   clang -O2 benchmarks/tasking/fiber_floor.c runtime/rt_fiber.S \
 *         -Iruntime -o benchmarks/tasking/fiber_floor
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_fiber.h"

#if defined(_WIN32)
#include <windows.h>
static double now_ns(void) {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1e9 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

/* The floor harness is single-threaded, so the contexts are globals. The
 * real scheduler keeps the current fiber in a thread-local and hands the
 * task body its arguments through the fiber; that changes no cost here. */
static pli_fiber parent_fib;
static pli_fiber child_fib;
static volatile long long sink;

/* The task body: the cheapest possible one - touch a global so the work
 * is not optimised away. */
static void do_work(void *arg) {
  (void)arg;
  sink++;
}

/* The fiber entry point. The assembly jumps to it, so it takes no
 * register arguments. */
static void fiber_entry(void) {
  do_work(NULL);
  /* The join: switch back to the parent, which resumes right after the
   * switch that started us. */
  pli_fiber_switch(&child_fib, &parent_fib);
  /* The parent never switches back to us, so this is unreachable. */
  fprintf(stderr, "fiber_floor: a task body returned\n");
  exit(2);
}

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 1000000;
  size_t stack_size = (size_t)8 << 20;
  void *stack = malloc(stack_size);
  if (!stack) {
    fprintf(stderr, "fiber_floor: out of memory\n");
    return 1;
  }
  void *stack_top = (char *)stack + stack_size;

  /* Warm up: the first switch pulls the stack in. */
  for (long i = 0; i < 1000; ++i) {
    pli_fiber_start(&child_fib, stack_top, (void *)fiber_entry);
    pli_fiber_switch(&parent_fib, &child_fib);
  }

  double t0 = now_ns();
  for (long i = 0; i < n; ++i) {
    pli_fiber_start(&child_fib, stack_top, (void *)fiber_entry);
    pli_fiber_switch(&parent_fib, &child_fib);
  }
  double t1 = now_ns();

  /* The round trip is a correctness test as well as a measurement: the
   * compiler keeps the loop counter and the other live values in the
   * callee-saved registers across the switch, so a switch that failed to
   * preserve them would corrupt the loop long before the count was wrong. */
  if (sink != n + 1000) {
    fprintf(stderr, "fiber_floor: FAIL the switch lost a register (sink=%lld, want %ld)\n",
            sink, n + 1000);
    return 1;
  }

  double per = (t1 - t0) / (double)n;
  printf("fiber_floor: %ld start+join round trips, %.1f ns each (%.3f ms total)\n",
         n, per, (t1 - t0) / 1e6);
  printf("fiber_floor: sink=%lld\n", sink);
  free(stack);
  return 0;
}
