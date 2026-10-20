/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* ADR-234 benchmark W1b (C reference): the task lifecycle with OS
   threads: create a thread that does nothing and join it, N times over.
   The same work runs in the PL/I and the Go baselines (task_loop.pli,
   task_loop.go). */
#include <stdio.h>

#define TASKS 20000

#if defined(_WIN32)
#include <windows.h>

static DWORD WINAPI worker(LPVOID arg) {
  (void)arg;
  return 0;
}

int main(void) {
  int i;
  for (i = 0; i < TASKS; ++i) {
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
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
  int i;
  for (i = 0; i < TASKS; ++i) {
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);
  }
  printf("done\n");
  return 0;
}
#endif
