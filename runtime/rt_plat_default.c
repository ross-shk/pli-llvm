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
long long pli_plat_now_us(void) {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return (long long)(t.QuadPart * 1000000 / f.QuadPart);
}
size_t pli_plat_page_size(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
}
/* Read standard input without the stdio buffer: the input broker reads on
 * its own thread, and the bytes it hands to the tasks must be the same
 * bytes the program would have read - so the stdio buffer must not be in
 * the way. */
int pli_plat_stdin_read(char *buf, int n) {
  if (n <= 0)
    return 0;
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  if (h == NULL || h == INVALID_HANDLE_VALUE)
    return 0;
  DWORD got = 0;
  if (!ReadFile(h, buf, (DWORD)n, &got, NULL))
    return 0; /* a console with no input: the read fails */
  return (int)got;
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
 * The stacks come from an *arena*: one reservation, carved into slots.
 * A program with twenty thousand tasks would otherwise make twenty
 * thousand reservations, and the reservation itself is a kernel call
 * that costs about a microsecond each. One arena costs one call. The
 * slots are handed out from a free list, and a slot's guard page sits
 * between it and the next one, so a stack that runs out faults into its
 * own guard and is reported as an overflow.
 *
 * The arena is listed in a lock-free list: the handler may run at any
 * moment, so it takes no lock. The list only grows — a stack that is
 * released stays in the list, and committing a released stack fails, which
 * leaves the fault to the default handler (a real crash, reported as
 * such). */
#define RT_STACK_WINDOW (64u * 1024u) /* the commit granularity */
#define RT_STACK_MARGIN (64u * 1024u) /* committed below the fault, for the
                                        * handler's own frame */
#define RT_ARENA_SLOTS 64              /* slots per arena reservation */
typedef struct RtStack {
  struct RtStack *next;
  char *base;  /* the usable base; the guard page is the page below it */
  size_t size; /* the usable size */
  char *low;   /* the lowest committed address */
} RtStack;

static RtStack *rt_stacks = NULL;
static INIT_ONCE rt_stacks_once = INIT_ONCE_STATIC_INIT;
static PVOID rt_veh = NULL;

/* The arena: one reservation, carved into slots.
 *
 * A program with twenty thousand tasks would otherwise make twenty thousand
 * reservations, and a reservation is a kernel call that costs about a
 * microsecond. One arena costs one call, and the slots come out of it
 * without one.
 *
 * The arena is only used for stacks of one size at a time: the first stack
 * of a given size creates the arena, and later stacks of that size take
 * slots from it. The arena's slots are equal-sized, so a faulting address
 * names its slot by arithmetic - the handler searches no list.
 *
 * A slot is laid out from its bottom up, because that is the direction a
 * stack grows:
 *
 *     +----------------------+  slot_base + slot
 *     |  the stack's top     |  the fiber starts here
 *     |  (grows downward)    |
 *     |                      |
 *     |  the usable region  |
 *     +----------------------+  slot_base + page
 *     |  the guard page      |  a stack that runs out lands here
 *     +----------------------+  slot_base
 *
 * The guard page is the slot's first page, below the usable region: a
 * stack that runs out of room lands there, and it never reaches into the
 * next slot. The usable part is committed from the top down, in windows,
 * as the stack grows into it.
 *
 * The free list is shared by the workers, so it is guarded by a spin lock:
 * the critical section is a couple of instructions, and a stack allocation
 * is not the hot path (the fiber pool reuses the stacks). */
typedef struct RtArena {
  struct RtArena *next;
  char *base;        /* the reservation */
  size_t bytes;      /* its size */
  size_t slot;       /* the slot size: the usable size plus the guard page */
  size_t usable;     /* the usable size of a slot */
  size_t page;       /* the guard page size */
  volatile LONG *free_slots; /* the free slot indices, LIFO */
  char **lows;       /* per slot: the lowest committed address */
  int free_top;      /* how many are on the free list */
  int free_count;    /* the free list's capacity */
  volatile LONG lock; /* the free list's spin lock */
} RtArena;

static RtArena *rt_arenas = NULL;
static INIT_ONCE rt_arena_once = INIT_ONCE_STATIC_INIT;
static volatile LONG rt_arena_lock = 0;

static LONG CALLBACK rt_stack_veh(EXCEPTION_POINTERS *ep);

static BOOL CALLBACK rt_stacks_init_cb(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o;
  (void)p;
  (void)c;

  rt_veh = AddVectoredExceptionHandler(1, rt_stack_veh);
  return TRUE;
}

static BOOL CALLBACK rt_arena_init_cb(PINIT_ONCE o, PVOID p, PVOID *c) {
  (void)o;
  (void)p;
  (void)c;
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

/* The arena that holds the faulting address, or NULL. The slots are
 * equal-sized, so the slot is found by arithmetic. */
static RtArena *rt_arena_of(char *addr) {
  for (RtArena *a = rt_arenas; a; a = a->next) {
    if (addr >= a->base && addr < a->base + a->bytes)
      return a;
  }
  return NULL;
}

/* The stack that owns the faulting address, or NULL. */
static RtStack *rt_stack_of(char *addr) {
  RtArena *a = rt_arena_of(addr);
  if (!a)
    return NULL;
  size_t off = (size_t)(addr - a->base);
  size_t idx = off / a->slot;
  char *slot_base = a->base + idx * a->slot;
  /* The slot's guard page sits at its low end, the usable stack above it. */
  return (RtStack *)(void *)(a->base + idx * a->slot + a->page);
}

static LONG CALLBACK rt_stack_veh(EXCEPTION_POINTERS *ep) {
  EXCEPTION_RECORD *r = ep->ExceptionRecord;
  if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  /* ExceptionAddress is the faulting *instruction*; the address that was
   * actually accessed is the first extra information field. */
  char *fault = (char *)r->ExceptionInformation[1];
  size_t page = pli_plat_page_size();
  RtArena *a = rt_arena_of(fault);
  if (a) {
    size_t idx = (size_t)(fault - a->base) / a->slot;
    char *slot_base = a->base + idx * a->slot;
    char *usable = slot_base + a->page;
    if (fault >= usable && fault < usable + a->usable) {
      /* A touch inside the stack. The committed region is [low, top) and
       * the stack grows down from the top, so the faulting page is below
       * it: commit the gap [grow, low), which covers the fault and a
       * margin below it - the handler itself runs on this stack, so its own
       * frame has to be in committed memory too. The margin is clipped to
       * the slot, so the commit never reaches into the next slot. */
      char *from = (char *)((uintptr_t)fault & ~(uintptr_t)(page - 1));
      char *low = a->lows[idx]; /* the lowest committed address */
      char *grow = from - (long)RT_STACK_MARGIN;
      if (grow < usable)
        grow = usable;   /* the slot's floor: the guard page is below it */
      if (grow > from)
        grow = from;     /* the faulting page must be in the commit */
      if (grow < low) {
        if (!VirtualAlloc(grow, (size_t)(low - grow), MEM_COMMIT, PAGE_READWRITE))
          return EXCEPTION_CONTINUE_SEARCH;
        a->lows[idx] = grow;
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (fault >= slot_base && fault < slot_base + a->page) {
      /* The guard page: the task's stack ran out. */
      fprintf(stderr, "TASK: stack overflow (the task's stack ran out)\n");
      fflush(stderr);
      return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

/* The arena that holds stacks of this size, or NULL. The list is guarded by
 * a spin lock: creating an arena is rare, and the lock is held for a few
 * instructions. */
static RtArena *rt_arena_new(size_t usable, size_t page) {
  size_t slot = usable + page;
  size_t bytes = slot * RT_ARENA_SLOTS;
  char *base = (char *)VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
  if (!base)
    return NULL;
  RtArena *a = (RtArena *)malloc(sizeof(RtArena));
  if (!a) {
    VirtualFree(base, 0, MEM_RELEASE);
    return NULL;
  }
  a->free_slots = (volatile LONG *)malloc(sizeof(LONG) * RT_ARENA_SLOTS);
  a->lows = (char **)malloc(sizeof(char *) * RT_ARENA_SLOTS);
  if (!a->free_slots || !a->lows) {
    free(a->free_slots);
    free(a->lows);
    free(a);
    VirtualFree(base, 0, MEM_RELEASE);
    return NULL;
  }
  a->base = base;
  a->bytes = bytes;
  a->slot = slot;
  a->usable = usable;
  a->page = page;
  /* Every slot starts free, with nothing committed yet. The watermark of a
   * slot is the top of its usable region: the stack grows down from there. */
  for (int i = 0; i < RT_ARENA_SLOTS; ++i) {
    a->free_slots[i] = i;
    a->lows[i] = base + (size_t)(i + 1) * slot;
  }
  a->free_top = RT_ARENA_SLOTS;
  a->free_count = RT_ARENA_SLOTS;
  a->lock = 0;
  while (InterlockedCompareExchange(&rt_arena_lock, 1, 0) != 0)
    Sleep(0);
  a->next = rt_arenas;
  rt_arenas = a;
  InterlockedExchange(&rt_arena_lock, 0);
  return a;
}

/* The newest arena that holds stacks of this size, or NULL. The list is
 * guarded by a spin lock: creating an arena is rare, and the lock is held
 * for a few instructions. */
static RtArena *rt_arena_get(size_t usable, size_t page) {
  (void)page;
  while (InterlockedCompareExchange(&rt_arena_lock, 1, 0) != 0)
    Sleep(0);
  RtArena *found = NULL;
  for (RtArena *a = rt_arenas; a; a = a->next) {
    if (a->usable == usable) {
      found = a;
      break;
    }
  }
  InterlockedExchange(&rt_arena_lock, 0);
  return found;
}

/* Take a free slot from the arena, or -1 when it is full. */
static long rt_arena_take(RtArena *a) {
  while (InterlockedCompareExchange(&a->lock, 1, 0) != 0)
    Sleep(0);
  long index = -1;
  if (a->free_top > 0) {    index = (long)a->free_slots[--a->free_top];
  }
  InterlockedExchange(&a->lock, 0);
  return index;
}

/* Take a slot from an arena of this size, making a new arena when they are
 * all full. The arenas are searched newest first: the newest one is the one
 * most likely to have room, and the others are tried in turn. */
static long rt_arena_take_slot(size_t usable, size_t page, RtArena **out_arena) {
  RtArena *a = rt_arena_get(usable, page);
  if (a) {
    long index = rt_arena_take(a);
    if (index >= 0) {
      *out_arena = a;
      return index;
    }
  }
  /* Every arena of this size is full: make another one. */
  a = rt_arena_new(usable, page);
  if (!a)
    return -1;
  long index = rt_arena_take(a);
  if (index < 0)
    return -1;
  *out_arena = a;
  return index;
}

/* Give a slot back to its arena. */
static void rt_arena_give(RtArena *a, size_t index) {
  while (InterlockedCompareExchange(&a->lock, 1, 0) != 0)
    Sleep(0);
  if (a->free_top < a->free_count)
    a->free_slots[a->free_top++] = (LONG)index;
  InterlockedExchange(&a->lock, 0);
}

int pli_plat_stack_alloc(size_t size, void **out_base) {
  size_t page = pli_plat_page_size();
  size = (size + page - 1) & ~(page - 1);
  InitOnceExecuteOnce(&rt_stacks_once, rt_stacks_init_cb, NULL, NULL);
  InitOnceExecuteOnce(&rt_arena_once, rt_arena_init_cb, NULL, NULL);
  /* The arena holds the stacks: one reservation, many slots. A new arena
   * is made when the current one is full. */
  RtArena *a = NULL;
  long index = rt_arena_take_slot(size, page, &a);
  if (index < 0)
    return -1;
  char *slot_base = a->base + (size_t)index * a->slot;
  /* The usable region sits above the slot's guard page, so the fiber's
   * stack (which starts at its top and grows down) stays inside the slot:
   * the top is at the slot's end, and the guard page is below the usable
   * region. */
  char *usable = slot_base + a->page;
  size_t window = RT_STACK_WINDOW < size ? RT_STACK_WINDOW : size;
  if (!VirtualAlloc(usable + size - window, window, MEM_COMMIT, PAGE_READWRITE))
    return -1;
  a->lows[index] = usable + size - window;
  *out_base = usable;
  return 0;
}
void pli_plat_stack_free(void *base, size_t size) {
  (void)size;
  if (!base)
    return;
  /* The slot goes back to its arena's free list; the pages stay committed,
   * so the next task that takes the slot does not pay for them again. */
  char *bp = (char *)base;
  for (RtArena *a = rt_arenas; a; a = a->next) {
    if (bp >= a->base && bp < a->base + a->bytes) {
      rt_arena_give(a, (size_t)(bp - a->base) / a->slot);
      return;
    }
  }
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
#include <signal.h>
#include <string.h>
#if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

/* The stack overflow diagnostic (ADR-237).
 *
 * A fiber stack has a guard page below it, so a stack that runs out of
 * room faults on an inaccessible page. Windows has a vectored handler for
 * this; POSIX has a signal handler, and it needs an alternate stack: the
 * faulting stack is full, so the handler cannot run on it.
 *
 * The handler tells a stack overflow from any other fault by the address:
 * the guard pages are remembered here, and a fault inside one of them is
 * an overflow. Every other fault is left to the default handler, so a real
 * bug still crashes the program the way it should.
 *
 * The guard list is append-only and lock-free: the handler may run at any
 * moment, so it takes no lock. A released stack stays in the list (like the
 * Windows one), and a fault in a released guard page is reported as an
 * overflow, which is the right answer anyway. */
#define RT_GUARD_MAX 4096
static char *rt_guards[RT_GUARD_MAX];
static size_t rt_guard_bytes[RT_GUARD_MAX];
static volatile int rt_guard_count = 0;
static volatile int rt_guard_ready = 0;

static void rt_guard_add(void *p, size_t bytes) {
  int i = __atomic_fetch_add(&rt_guard_count, 1, __ATOMIC_RELAXED);
  if (i < RT_GUARD_MAX) {
    rt_guards[i] = (char *)p;
    rt_guard_bytes[i] = bytes;
  }
}

static void rt_guard_remove(void *p) {
  for (int i = 0; i < rt_guard_count && i < RT_GUARD_MAX; ++i) {
    if (rt_guards[i] == (char *)p) {
      rt_guards[i] = NULL;
      rt_guard_bytes[i] = 0;
      return;
    }
  }
}

/* Is the address inside one of the guard pages? */
static int rt_guard_holds(void *addr) {
  for (int i = 0; i < rt_guard_count && i < RT_GUARD_MAX; ++i) {
    if (!rt_guards[i])
      continue;
    if ((char *)addr >= rt_guards[i] &&
        (char *)addr < rt_guards[i] + rt_guard_bytes[i])
      return 1;
  }
  return 0;
}

/* The handler runs on an alternate stack: the fiber's stack is full. The
 * alternate stack is per thread, and a thread installs one the first time
 * it allocates a fiber stack. */
static __thread char *rt_alt_stack = NULL;

static void rt_alt_stack_install(void) {
  if (rt_alt_stack)
    return;
  size_t page = pli_plat_page_size();
  size_t bytes = 64u * 1024u;
  stack_t ss;
  ss.ss_sp = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ss.ss_sp == MAP_FAILED)
    return;
  ss.ss_size = bytes;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, NULL) != 0) {
    munmap(ss.ss_sp, bytes);
    return;
  }
  rt_alt_stack = (char *)ss.ss_sp;
}

static void rt_segv_handler(int sig, siginfo_t *info, void *ctx) {
  (void)ctx;
  if (info && rt_guard_holds(info->si_addr)) {
    static const char msg[] =
        "TASK: stack overflow (the task's stack ran out)\n";
    ssize_t n = write(2, msg, sizeof(msg) - 1);
    (void)n;
    _exit(134);
  }
  /* Not a stack overflow: restore the default handler and let the fault
   * happen again, so the program dies the way it normally would. */
  signal(sig, SIG_DFL);
  raise(sig);
}

/* Install the handler and the alternate stack. Called when the first fiber
 * stack is allocated, which is on a worker thread. */
static void rt_fault_handler_install(void) {
  if (__atomic_exchange_n(&rt_guard_ready, 1, __ATOMIC_RELAXED))
    return;
  rt_alt_stack_install();
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = rt_segv_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}

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
/* Read standard input without the stdio buffer: the input broker reads on
 * its own thread, and the bytes it hands to the tasks must be the same
 * bytes the program would have read - so the stdio buffer must not be in
 * the way. */
int pli_plat_stdin_read(char *buf, int n) {
  if (n <= 0)
    return 0;
  for (;;) {
    ssize_t got = read(0, buf, (size_t)n);
    if (got >= 0)
      return (int)got;
    if (errno == EINTR)
      continue; /* a signal: read again */
    return -1;
  }
}
long long pli_plat_now_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
size_t pli_plat_page_size(void) {
  long p = sysconf(_SC_PAGESIZE);
  return p > 0 ? (size_t)p : 4096;
}
int pli_plat_stack_alloc(size_t size, void **out_base) {
  size_t page = pli_plat_page_size();
  size = (size + page - 1) & ~(page - 1);
  /* The fault handler and the alternate stack it runs on: the first
   * fiber stack on this thread installs them. */
  rt_fault_handler_install();
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
  /* Remember the guard page, so the fault handler can tell a stack
   * overflow from any other fault (ADR-237). */
  rt_guard_add(p, page);
  *out_base = (char *)p + page;
  return 0;
}
void pli_plat_stack_free(void *base, size_t size) {
  size_t page = pli_plat_page_size();
  size = (size + page - 1) & ~(page - 1);
  rt_guard_remove((char *)base - page);
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
