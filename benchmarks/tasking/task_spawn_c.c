/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* ADR-234 benchmark W1 (C reference): fan-out / fan-in with OS threads.
   The same work runs in the PL/I and the Go baselines
   (task_spawn.pli, task_spawn.go). */
#include <stdio.h>

#define TASKS 20000

#if defined(_WIN32)
#include <windows.h>

static HANDLE th[TASKS];

static DWORD WINAPI worker(LPVOID arg) {
  (void)arg;
  return 0;
}

int main(void) {
  int i;
  for (i = 0; i < TASKS; ++i) {
    th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    /* Resource exhaustion must fail loudly: a fan-out that silently started
       fewer threads would look like a win. */
    if (th[i] == NULL) { fprintf(stderr, "CreateThread failed\n"); return 1; }
  }
  for (i = 0; i < TASKS; ++i) {
    WaitForSingleObject(th[i], INFINITE);
    CloseHandle(th[i]);
  }
  printf("done\n");
  return 0;
}
#else
#include <pthread.h>

static void *worker(void *arg) {
  (void)arg;
  return NULL;
}

int main(void) {
  pthread_t th[TASKS];
  int i;
  for (i = 0; i < TASKS; ++i) {
    /* Resource exhaustion must fail loudly: a fan-out that silently started
       fewer threads would look like a win. */
    if (pthread_create(&th[i], NULL, worker, NULL) != 0) {
      fprintf(stderr, "pthread_create failed at %d\n", i);
      return 1;
    }
  }
  for (i = 0; i < TASKS; ++i) {
    if (pthread_join(th[i], NULL) != 0) {
      fprintf(stderr, "pthread_join failed at %d\n", i);
      return 1;
    }
  }
  printf("done\n");
  return 0;
}
#endif
