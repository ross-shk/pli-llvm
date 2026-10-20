/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* runtime/rt_plat_default.c — default platform TU: the host half of the split
 * runtime. Implements every symbol declared in pli_rt_plat.h against the host
 * libc/pthreads (or Win32), so the portable TUs stay branch-free. Bodies are
 * the former pli_rt_port.h wrappers (ADR-221) plus the sync/pli_thread.h
 * backends, moved out-of-line unchanged. This is the TU --rt-platform
 * replaces: a custom platform provides these same symbols and the portable
 * blob links against it untouched. Self-contained by contract: names only
 * libc and pli_rt_plat.h here. */
#include "pli_rt_plat.h"

#include <stdarg.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

/* --- allocation: direct libc (the portable TUs must not name these) */
void *pli_plat_malloc(size_t n) { return malloc(n); }
void *pli_plat_calloc(size_t n, size_t size) { return calloc(n, size); }
void *pli_plat_realloc(void *p, size_t n) { return realloc(p, n); }
void pli_plat_free(void *p) { free(p); }

/* --- exit/abort raw ends (flushing happens in portable pli_exit/pli_abort) */
void pli_plat_exit(int code) { exit(code); }
void pli_plat_abort(void) { abort(); }

/* --- process spawn: one branch per platform, wait-status dance included */
FILE *pli_plat_popen(const char *cmd, const char *mode) {
#if defined(_WIN32)
  return _popen(cmd, mode);
#else
  return popen(cmd, mode);
#endif
}
int pli_plat_pclose(FILE *fp) {
#if defined(_WIN32)
  return _pclose(fp);
#else
  int raw = pclose(fp);
  if (raw == -1)
    return -1;
  if (WIFEXITED(raw))
    return WEXITSTATUS(raw);
  return -1;
#endif
}

/* --- calendar time: re-entrant POSIX, locked _s on Windows */
void pli_plat_localtime(const time_t *t, struct tm *out) {
#if defined(_WIN32)
  localtime_s(out, t);
#else
  localtime_r(t, out);
#endif
}

/* --- FILE I/O: mode strings ("r"/"w" vs "rb"/"wb") chosen by the callers */
FILE *pli_plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int pli_plat_fclose(FILE *fp) { return fclose(fp); }
size_t pli_plat_fread(void *p, size_t size, size_t n, FILE *fp) {
  return fread(p, size, n, fp);
}
size_t pli_plat_fwrite(const void *p, size_t size, size_t n, FILE *fp) {
  return fwrite(p, size, n, fp);
}
int pli_plat_fgetc(FILE *fp) { return fgetc(fp); }
int pli_plat_getchar(void) { return getchar(); }
int pli_plat_fputc(int c, FILE *fp) { return fputc(c, fp); }
int pli_plat_fflush(FILE *fp) { return fflush(fp); }
int pli_plat_ferror(FILE *fp) { return ferror(fp); }
/* Printf via vfprintf: the ... crosses the seam, so __builtin_va_list never
 * appears in the portable blob's ABI. */
int pli_plat_fprintf(FILE *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(fp, fmt, ap);
  va_end(ap);
  return n;
}

/* --- the tasking model (ADR-234): the environment, the machine, the clock */
const char *pli_plat_getenv(const char *name) {
  const char *v = getenv(name);
  return v ? v : "";
}

/* --- threading backends (single selection block; bodies ex pli_thread.h) */
#if defined(_WIN32)
/* Windows backend (first half). */
#include <process.h>
/* The tasking model (ADR-234): the machine size, a monotonic clock, the
 * page size, and a lazily committed fiber stack with a guard page below. */
int pli_plat_cpu_count(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (int)si.dwNumberOfProcessors;
}
long long pli_plat_now_ms(void) { return (long long)GetTickCount64(); }
size_t pli_plat_page_size(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
}

/* The fiber stack: address space, not memory.
 *
 * The ceiling is reserved, not committed: a task that never touches its
 * stack costs one page, not the whole ceiling, so a program with ten
 * thousand tasks still fits in a small address space. The pages are
 * committed as the stack grows into them, by the vectored handler below:
 * it recognises a fault inside a fiber stack, commits the next window
 * below the committed part and resumes the faulting instruction. A fault
 * in the guard page below the stack is a stack overflow, and it is
 * reported as one.
 *
 * The stacks are listed in a lock-free list: the handler may run at any
 * moment, so it takes no lock. The list only grows — a stack that is
 * released stays in the list, and committing a released stack fails, which
 * leaves the fault to the default handler (a real crash, reported as
 * such). */
#define RT_STACK_WINDOW (64u * 1024u) /* the commit granularity */
#define RT_STACK_MARGIN (64u * 1024u) /* committed below the fault, for the
                                       * handler's own frame */

typedef struct RtStack {
  struct RtStack *next;
  char *base;  /* the usable base; the guard page is the page below it */
  size_t size; /* the usable size */
  char *low;   /* the lowest committed address */
} RtStack;

static RtStack *rt_stacks = NULL;
static INIT_ONCE rt_stacks_once = INIT_ONCE_STATIC_INIT;
static PVOID rt_veh = NULL;

static LONG CALLBACK rt_stack_veh(EXCEPTION_POINTERS *ep);

static BOOL CALLBACK rt_stacks_init_cb(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o;
  (void)p;
  (void)c;
  rt_veh = AddVectoredExceptionHandler(1, rt_stack_veh);
  return TRUE;
}

/* Commit [from, to) of the stack and remember the new low watermark. */
static int rt_stack_commit(RtStack *s, char *from, char *to) {
  if (to <= from)
    return 0;
  /* MEM_COMMIT at a given address commits the reserved pages there. */
  void *p = VirtualAlloc(from, (size_t)(to - from), MEM_COMMIT, PAGE_READWRITE);
  if (!p)
    return -1;
  s->low = from;
  return 0;
}

static LONG CALLBACK rt_stack_veh(EXCEPTION_POINTERS *ep) {
  EXCEPTION_RECORD *r = ep->ExceptionRecord;
  if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  /* ExceptionAddress is the faulting *instruction*; the address that was
   * actually accessed is the first extra information field. */
  char *fault = (char *)r->ExceptionInformation[1];
  size_t page = pli_plat_page_size();
  for (RtStack *s = rt_stacks; s; s = s->next) {
    if (fault >= s->base && fault < s->base + s->size) {
      /* A touch inside the stack: commit the gap down to the fault, and a
       * margin below it: the handler itself runs on this stack, so its own
       * frame has to be in committed memory too. */
      char *from = (char *)((uintptr_t)fault & ~(uintptr_t)(page - 1));
      char *grow = from - (long)RT_STACK_MARGIN;
      if (grow < s->base)
        grow = s->base;
      if (rt_stack_commit(s, grow, s->low) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (fault >= s->base - (long)page && fault < s->base) {
      /* The guard page: the task's stack ran out. */
      fprintf(stderr, "TASK: stack overflow (the task's stack ran out)\n");
      fflush(stderr);
      return EXCEPTION_CONTINUE_SEARCH;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

int pli_plat_stack_alloc(size_t size, void **out_base) {
  size_t page = pli_plat_page_size();
  size = (size + page - 1) & ~(page - 1);
  InitOnceExecuteOnce(&rt_stacks_once, rt_stacks_init_cb, NULL, NULL);
  /* Reserve the ceiling: no commit charge, so the cost of an idle task is
   * address space, not memory. */
  char *p = (char *)VirtualAlloc(NULL, size + page, MEM_RESERVE, PAGE_NOACCESS);
  if (!p)
    return -1;
  RtStack *s = (RtStack *)malloc(sizeof(RtStack));
  if (!s) {
    VirtualFree(p, 0, MEM_RELEASE);
    return -1;
  }
  s->base = p + page;
  s->size = size;
  s->low = s->base + size;
  /* Commit the top window: the fiber starts there. */
  size_t window = RT_STACK_WINDOW < size ? RT_STACK_WINDOW : size;
  if (rt_stack_commit(s, s->low - window, s->low) != 0) {
    VirtualFree(p, 0, MEM_RELEASE);
    free(s);
    return -1;
  }
  s->next = rt_stacks;
  rt_stacks = s;
  *out_base = s->base;
  return 0;
}
void pli_plat_stack_free(void *base, size_t size) {
  (void)size;
  if (!base)
    return;
  VirtualFree((char *)base - pli_plat_page_size(), 0, MEM_RELEASE);
}
/* Mutual exclusion around one shared EVENT/task table. */
void pli_mutex_init(pli_mutex *m) { InitializeCriticalSection(m); }
void pli_mutex_lock(pli_mutex *m) { EnterCriticalSection(m); }
void pli_mutex_unlock(pli_mutex *m) { LeaveCriticalSection(m); }
/* Recursive mutex: a CRITICAL_SECTION is recursive by nature. */
void pli_mutex_init_recursive(pli_mutex *m) { InitializeCriticalSection(m); }
/* Condition broadcast/wait paired with the mutex above. */
void pli_cond_init(pli_cond *c) { InitializeConditionVariable(c); }
void pli_cond_wait(pli_cond *c, pli_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
int pli_cond_timedwait(pli_cond *c, pli_mutex *m, long long timeout_ms) {
  /* A negative timeout waits forever: the scheduler parks an idle worker
   * with one, and a zero timeout would turn the park into a spin. */
  if (timeout_ms < 0)
    return SleepConditionVariableCS(c, m, INFINITE) ? 1 : 0;
  return SleepConditionVariableCS(c, m, (DWORD)timeout_ms) ? 1 : 0;
}
void pli_cond_signal(pli_cond *c) { WakeConditionVariable(c); }
void pli_cond_broadcast(pli_cond *c) { WakeAllConditionVariable(c); }
/* One-time initialisation for mutexes/conds that have no static initializer. */
/* Adapter carrying a `void(*)(void)` through InitOnceExecuteOnce. */
struct pli_once_ctx {
  void (*fn)(void);
};
static BOOL CALLBACK pli_once_trampoline(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o;
  (void)c;
  ((struct pli_once_ctx *)p)->fn();
  return TRUE;
}
void pli_call_once(pli_once *o, void (*fn)(void)) {
  /* Synchronous: the stack argument outlives the call. */
  struct pli_once_ctx ctx;
  ctx.fn = fn;
  InitOnceExecuteOnce(o, pli_once_trampoline, &ctx, NULL);
}
/* Spawn a thread running fn(arg); returns 0 on success. */
/* Heap pair carrying `void *(*)(void *)` through _beginthreadex. */
/* _beginthreadex needs `unsigned __stdcall`; carry fn+arg on the heap. */
struct pli_spawn_ctx {
  void *(*fn)(void *);
  void *arg;
};
static unsigned __stdcall pli_spawn_trampoline(void *p) {
  struct pli_spawn_ctx *c = (struct pli_spawn_ctx *)p;
  void *(*fn)(void *) = c->fn;
  void *arg = c->arg;
  free(c);
  fn(arg);
  _endthreadex(0);
  return 0;
}
int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg) {
  struct pli_spawn_ctx *c = (struct pli_spawn_ctx *)malloc(sizeof(*c));
  if (!c)
    return -1;
  c->fn = fn;
  c->arg = arg;
  uintptr_t h = _beginthreadex(NULL, 0, pli_spawn_trampoline, c, 0, NULL);
  if (h == 0) {
    free(c);
    return -1;
  }
  *th = h;
  return 0;
}
void pli_thread_detach(pli_thread th) { CloseHandle((HANDLE)th); }
/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
void pli_sleep_ms(long long ms) {
  if (ms <= 0)
    return;
  Sleep((DWORD)ms);
}
#else
/* POSIX backend (second half). */
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif
/* The tasking model (ADR-234): the machine size, a monotonic clock, the
 * page size, and a lazily committed fiber stack with a guard page below. */
int pli_plat_cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}
long long pli_plat_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
size_t pli_plat_page_size(void) {
  long p = sysconf(_SC_PAGESIZE);
  return p > 0 ? (size_t)p : 4096;
}
int pli_plat_stack_alloc(size_t size, void **out_base) {
  size_t page = pli_plat_page_size();
  size = (size + page - 1) & ~(page - 1);
  /* PROT_READ|PROT_WRITE over an anonymous mapping: the pages are
   * demand-paged, so the reservation costs address space, not memory, and
   * the fiber stack grows into it without ever moving. */
  void *p = mmap(NULL, size + page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return -1;
  /* The guard page sits below the usable region: a fiber stack grows down
   * into it, so an overflow lands on an inaccessible page. */
  if (mprotect(p, page, PROT_NONE) != 0) {
    munmap(p, size + page);
    return -1;
  }
  *out_base = (char *)p + page;
  return 0;
}
void pli_plat_stack_free(void *base, size_t size) {
  size_t page = pli_plat_page_size();
  size = (size + page - 1) & ~(page - 1);
  munmap((char *)base - page, size + page);
}
/* Mutual exclusion around one shared EVENT/task table. */
void pli_mutex_init(pli_mutex *m) { pthread_mutex_init(m, NULL); }
void pli_mutex_lock(pli_mutex *m) { pthread_mutex_lock(m); }
void pli_mutex_unlock(pli_mutex *m) { pthread_mutex_unlock(m); }
/* Recursive mutex: the output serialiser takes its lock nested. */
void pli_mutex_init_recursive(pli_mutex *m) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
}
/* Condition broadcast/wait paired with the mutex above. */
void pli_cond_init(pli_cond *c) { pthread_cond_init(c, NULL); }
void pli_cond_wait(pli_cond *c, pli_mutex *m) { pthread_cond_wait(c, m); }
int pli_cond_timedwait(pli_cond *c, pli_mutex *m, long long timeout_ms) {
  /* A negative timeout waits forever: the scheduler parks an idle worker
   * with one, and a deadline in the past would turn the park into a spin. */
  if (timeout_ms < 0)
    return pthread_cond_wait(c, m) == 0 ? 1 : 0;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += (time_t)(timeout_ms / 1000);
  ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec += 1;
  }
  return pthread_cond_timedwait(c, m, &ts) == 0 ? 1 : 0;
}
void pli_cond_signal(pli_cond *c) { pthread_cond_signal(c); }
void pli_cond_broadcast(pli_cond *c) { pthread_cond_broadcast(c); }
/* One-time initialisation for mutexes/conds that have no static initializer. */
void pli_call_once(pli_once *o, void (*fn)(void)) { pthread_once(o, fn); }
/* Spawn a thread running fn(arg); returns 0 on success. */
int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg) {
  return pthread_create(th, NULL, fn, arg);
}
void pli_thread_detach(pli_thread th) { pthread_detach(th); }
/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
void pli_sleep_ms(long long ms) {
  struct timespec ts;
  if (ms <= 0)
    return;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    ;
}
#endif

/* --- shared wrapper (platform-independent; was the header's tail) */
/* Spawn a detached thread running fn(arg); returns 0 on success. */
int pli_spawn_detached(void *(*fn)(void *), void *arg) {
  pli_thread th;
  if (pli_spawn(&th, fn, arg) != 0)
    return -1;
  pli_thread_detach(th);
  return 0;
}
