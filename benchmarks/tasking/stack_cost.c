/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* stack_cost.c — the per-stack cost of the fiber stack's commit policy.
 *
 * ADR-313 commits the top 64 KiB chunk of a new fiber stack; ADR-324
 * commits only its top page. This measures what that difference buys: a
 * PROT_NONE arena of slots, an mprotect of the top chunk or the top page,
 * and the first touch a fiber makes when it starts. It is a microbenchmark
 * for the platform path, not a tasking gate.
 *
 * Build and run (from the repo root, POSIX):
 *   cc -O2 benchmarks/tasking/stack_cost.c -o benchmarks/tasking/stack_cost
 *   ./benchmarks/tasking/stack_cost
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define N 20000

static long long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Reserve N slots of *chunk_pages* plus a guard, commit *commit_pages* at
 * the top of each, and optionally touch the first page the fiber uses. */
static void run(const char *name, size_t chunk_pages, size_t commit_pages,
                int touch) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  size_t slot = (chunk_pages + 1) * page;
  char *base = mmap(NULL, slot * N, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
  if (base == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  long long t0 = now_ns();
  for (size_t i = 0; i < N; ++i) {
    char *top = base + i * slot + slot;
    if (mprotect(top - commit_pages * page, commit_pages * page,
                 PROT_READ | PROT_WRITE) != 0) {
      perror("mprotect");
      exit(1);
    }
    if (touch)
      *(volatile char *)(top - page) = 1;
  }
  long long t1 = now_ns();
  printf("%-30s %8.0f ns/stack\n", name, (double)(t1 - t0) / N);
  munmap(base, slot * N);
}

int main(void) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  printf("page = %zu, stacks = %d\n", page, N);
  run("top chunk committed + touch", 4, 3, 1);
  run("top page committed + touch", 4, 1, 1);
  run("top page committed, no touch", 4, 1, 0);
  return 0;
}
