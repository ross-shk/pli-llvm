/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* runtime/platform/posix/rt_plat.c — default platform TU: the host half of the split
 * runtime. Implements every symbol declared in pli_rt_plat.h against the host
 * libc/pthreads (or Win32), so the portable TUs stay branch-free. Bodies are
 * the former pli_rt_port.h wrappers (ADR-221) plus the sync/pli_thread.h
 * backends, moved out-of-line unchanged. This is the TU --rt-platform
 * replaces: a custom platform provides these same symbols and the portable
 * blob links against it untouched. Self-contained by contract: names only
 * libc and pli_rt_plat.h here. */
#include "platform/pli_rt_plat.h"

#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>

/* --- allocation: direct libc (the portable TUs must not name these) */
void *pli_plat_malloc(size_t n) { return malloc(n); }
void *pli_plat_calloc(size_t n, size_t size) { return calloc(n, size); }
void *pli_plat_realloc(void *p, size_t n) { return realloc(p, n); }
void pli_plat_free(void *p) { free(p); }

/* --- exit/abort raw ends (flushing happens in portable pli_exit/pli_abort) */
void pli_plat_exit(int code) { exit(code); }
void pli_plat_abort(void) { abort(); }

void pli_plat_pause(void) {
#if defined(__i386__) || defined(__x86_64__)
  __builtin_ia32_pause();
#else
  /* noop on non-x86 targets */
#endif
}

void rt_fiber_unsupported(void) {
  pli_plat_fprintf(stderr, "TASK: this platform has no fiber backend\n");
  pli_plat_abort();
}

/* --- process spawn: one branch per platform, wait-status dance included */
FILE *pli_plat_popen(const char *cmd, const char *mode) {
  return popen(cmd, mode);
}
int pli_plat_pclose(FILE *fp) {
  int raw = pclose(fp);
  if (raw == -1)
    return -1;
  if (WIFEXITED(raw))
    return WEXITSTATUS(raw);
  return -1;
}

/* --- calendar time: re-entrant POSIX, locked _s on Windows */
time_t pli_plat_time(time_t *t) { return time(t); }
void pli_plat_localtime(const time_t *t, struct tm *out) {
  localtime_r(t, out);
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
/* Buffering control (for BUFFERED/UNBUFFERED OPEN options). */
int pli_plat_setvbuf(FILE *fp, char *buf, int mode, size_t size) {
#if defined(__APPLE__) || defined(__MACH__)
  /* macOS doesn't export setvbuf from libc.dylib; accept but no-op. */
  (void)fp; (void)buf; (void)mode; (void)size;
  return 0;
#else
  return setvbuf(fp, buf, mode, size);
#endif
}
/* Sequential record positioning (REWRITE/DELETE, rule (112)). POSIX uses the
 * ISO seek/tell pair: `long` is 64-bit on the LP64 targets, and a record file
 * larger than 2 GiB is out of scope for this slice. Windows needs the 64-bit
 * CRT calls (long is 32-bit there). */
int pli_plat_fseek(FILE *fp, long long off, int whence) {
  return fseek(fp, (long)off, whence);
}
long long pli_plat_ftell(FILE *fp) {
  return (long long)ftell(fp);
}
/* Truncate after a DELETE has shifted the tail down. The caller flushes
 * first; this flushes again because the FILE position must be settled. */
int pli_plat_ftruncate(FILE *fp, long long size) {
  if (fflush(fp) != 0)
    return -1;
  return ftruncate(fileno(fp), (off_t)size);
}
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
/* POSIX backend (second half). */
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>
#if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

/* The fiber stack: address space, not memory (ADR-313).
 *
 * The ceiling of a stack is reserved, not committed: a task that never
 * touches its stack costs one page, not the whole ceiling. The pages are
 * committed as the stack grows into them, by the signal handler below: a
 * fault below the committed part of the stack commits the chunks down to
 * the fault, and the stack grows on. A stack that reaches its bottom chunk
 * has run out of room, and it is reported as an overflow (ADR-237).
 *
 * The stacks come from an *arena*: one reservation carved into slots. A
 * program with twenty thousand live tasks would otherwise make twenty
 * thousand reservations, each with its own guard entry in a table that the
 * handler has to scan, and the reservation itself is a kernel call. One
 * arena costs one call, and the handler names a slot by arithmetic.
 *
 * A slot is laid out from its bottom up, because that is the direction a
 * stack grows:
 *
 *     +----------------------+  slot_base + slot
 *     |  the stack's top     |  the fiber starts here
 *     |  (grows downward)    |
 *     |  the usable region   |  the chunks, with a guard page
 *     |                      |  at the bottom of each
 *     +----------------------+  slot_base + page
 *     |  the slot's guard    |  a write below the stack stops here
 *     +----------------------+  slot_base
 *
 * The arena is one PROT_NONE reservation: a slot's pages are made readable
 * and writable only as the stack grows into them, so a fiber's ceiling
 * costs address space, never page-table entries.
 *
 * The arenas are listed in a lock-free list, because the handler may run at
 * any moment and takes no lock; the free lists are locked, because a slot
 * is handed out once per task that has no fiber to recycle. */
#define RT_STACK_CHUNK (64u * 1024u) /* the commit granularity */
#define RT_ARENA_SLOTS 64            /* slots per arena reservation */

typedef struct RtArena {
  struct RtArena *next;
  char *base;      /* the reservation */
  size_t bytes;    /* its size */
  size_t slot;     /* one slot: the usable region plus its guard page */
  size_t usable;   /* the usable size of a slot */
  size_t page;     /* the page size */
  int *free_slots; /* the free slot indices, LIFO */
  int free_top;    /* how many are on the free list */
} RtArena;

static RtArena *rt_arenas = NULL; /* the list head */
static pthread_mutex_t rt_stack_mu = PTHREAD_MUTEX_INITIALIZER;

/* The commit granularity: whole pages, so the chunk's bottom page can be
 * its guard. A page bigger than half a chunk widens the chunk. */
static size_t rt_stack_chunk_bytes(size_t page) {
  return RT_STACK_CHUNK < 2 * page ? 2 * page : RT_STACK_CHUNK;
}

/* The arena that holds the address, or NULL. */
static RtArena *rt_arena_of(char *addr) {
  for (RtArena *a = __atomic_load_n(&rt_arenas, __ATOMIC_ACQUIRE); a;
       a = a->next)
    if (addr >= a->base && addr < a->base + a->bytes)
      return a;
  return NULL;
}

/* A new arena of this slot size. The caller holds rt_stack_mu. */
static RtArena *rt_arena_new(size_t usable, size_t page) {
  size_t slot = usable + page;
  size_t bytes = slot * RT_ARENA_SLOTS;
  /* PROT_NONE: the reservation costs address space only. */
  char *base = (char *)mmap(NULL, bytes, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED)
    return NULL;
  RtArena *a = (RtArena *)pli_plat_malloc(sizeof(RtArena));
  int *free_slots = (int *)pli_plat_malloc(sizeof(int) * RT_ARENA_SLOTS);
  if (!a || !free_slots) {
    pli_plat_free(a);
    pli_plat_free(free_slots);
    munmap(base, bytes);
    return NULL;
  }
  a->base = base;
  a->bytes = bytes;
  a->slot = slot;
  a->usable = usable;
  a->page = page;
  a->free_slots = free_slots;
  /* Every slot starts free. */
  for (int i = 0; i < RT_ARENA_SLOTS; ++i)
    a->free_slots[i] = i;
  a->free_top = RT_ARENA_SLOTS;
  /* Publish the arena only when it is whole: the handler reads the list. */
  a->next = rt_arenas;
  __atomic_store_n(&rt_arenas, a, __ATOMIC_RELEASE);
  return a;
}

/* A free slot from an arena of this size, or NULL. */
static RtArena *rt_arena_take(size_t usable, size_t page, size_t *out_off) {
  pthread_mutex_lock(&rt_stack_mu);
  RtArena *a = rt_arenas;
  while (a && !(a->usable == usable && a->free_top > 0))
    a = a->next;
  if (!a)
    a = rt_arena_new(usable, page);
  if (a) {
    int index = a->free_slots[--a->free_top];
    *out_off = (size_t)index * a->slot;
  }
  pthread_mutex_unlock(&rt_stack_mu);
  return a;
}

/* Give a slot back to its arena. */
static void rt_arena_give(RtArena *a, size_t off) {
  pthread_mutex_lock(&rt_stack_mu);
  if (a->free_top < RT_ARENA_SLOTS)
    a->free_slots[a->free_top++] = (int)(off / a->slot);
  pthread_mutex_unlock(&rt_stack_mu);
}

/* Grow the stack that faulted, or say that it has run out.
 *
 * Returns 1 when the fault was a growth point and the chunk it landed in is
 * now committed (the kernel re-executes the faulting instruction when the
 * handler returns), 2 when the stack has run out, and 0 for any other fault
 * - a real bug still crashes the program the way it should.
 *
 * A fault is not always on a guard page: a procedure with a large frame
 * moves the stack pointer past several chunks before it touches one, so the
 * chunk that faults may be well below the committed region. Every chunk
 * between the fault and the top is committed, which covers both cases. */
static int rt_stack_fault(char *fault) {
  RtArena *a = rt_arena_of(fault);
  if (!a)
    return 0;
  size_t index = (size_t)(fault - a->base) / a->slot;
  char *usable = a->base + index * a->slot + a->page;
  char *top = usable + a->usable;
  if (fault >= top)
    return 0; /* outside the stack */
  size_t chunk = rt_stack_chunk_bytes(a->page);
  /* The bottom page of the usable region is the overflow guard: it stays
   * inaccessible even when the chunk above it is committed. A fault below
   * the usable region, or in that page, is the stack running out. */
  char *floor = usable + a->page;
  if (fault < floor)
    return 2; /* the guard page: the stack ran out */
  /* The chunks are counted from the top, because the stack starts at the
   * top and grows down; the fault names the chunk it landed in. */
  size_t n = ((size_t)(top - fault) - 1) / chunk;
  char *lo = top - (n + 1) * chunk;
  if (lo < floor)
    lo = floor; /* the bottom chunk: never commit its guard page */
  if (mprotect(lo, (size_t)(top - lo), PROT_READ | PROT_WRITE) != 0)
    return 0; /* nothing to be done for it: let the fault stand */
  return 1;
}

/* Set once the signal handler is in place (it is one per process). */
static volatile int rt_handler_ready = 0;

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
  int what = info ? rt_stack_fault((char *)info->si_addr) : 0;
  /* The stack grew: the faulting instruction is re-executed on return. */
  if (what == 1)
    return;
  if (what == 2) {
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

/* Install the handler and the alternate stack. The handler is one per
 * process, but the alternate stack is one per *thread* (sigaltstack is
 * thread-local), so every thread that may run fibers installs its own: a
 * worker started after the handler was installed would otherwise run the
 * handler on the full fiber stack and die instead of reporting. */
static void rt_fault_handler_install(void) {
  if (__atomic_exchange_n(&rt_handler_ready, 1, __ATOMIC_RELAXED))
    return;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = rt_segv_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}
/* The scheduler calls this on every thread that runs tasks. The handler is
 * installed once; the alternate stack is this thread's. */
void pli_plat_thread_init(void) {
  rt_fault_handler_install();
  rt_alt_stack_install();
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
int pli_plat_stack_alloc(size_t *size_inout, void **out_base) {
  size_t page = pli_plat_page_size();
  /* The usable region is a whole number of chunks: the handler finds a
   * chunk's guard page by arithmetic from the region's top, so a partial
   * chunk would put the guard in the wrong place. A chunk holds its own
   * guard page and the stack above it. */
  size_t chunk = rt_stack_chunk_bytes(page);
  size_t size = (*size_inout + chunk - 1) & ~(chunk - 1);
  if (size < chunk)
    size = chunk;
  size_t off = 0;
  RtArena *a = rt_arena_take(size, page, &off);
  if (!a)
    return -1;
  char *usable = a->base + off + page;
  char *top = usable + size;
  /* Commit only the page the fiber starts on; the rest of the top chunk
   * faults in through the handler when the task actually grows into it.
   * An idle task therefore commits one page, not a whole chunk. */
  if (mprotect(top - page, page, PROT_READ | PROT_WRITE) != 0) {
    rt_arena_give(a, off); /* nothing was committed: the slot is still free */
    return -1;
  }
  *size_inout = size; /* the usable size the caller gets */
  *out_base = usable;
  return 0;
}
void pli_plat_stack_free(void *base, size_t size) {
  (void)size;
  if (!base)
    return;
  /* The slot goes back to its arena: the pages stay committed, so the next
   * task that takes the slot does not pay for them again. */
  char *bp = (char *)base;
  for (RtArena *a = rt_arenas; a; a = a->next) {
    if (bp >= a->base && bp < a->base + a->bytes) {
      rt_arena_give(a, (size_t)(bp - a->base));
      return;
    }
  }
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


/* --- shared wrapper (platform-independent; was the header's tail) */
/* Spawn a detached thread running fn(arg); returns 0 on success. */
int pli_spawn_detached(void *(*fn)(void *), void *arg) {
  pli_thread th;
  if (pli_spawn(&th, fn, arg) != 0)
    return -1;
  pli_thread_detach(th);
  return 0;
}

/* Atomic access to a PL/I EVENT word (QR2.8). The word is a plain i32 in a
 * PL/I variable, so it cannot carry C11's _Atomic type; these calls are the
 * only way the runtime touches it.
 *
 * The runtime bitcode — this platform TU included — is built by clang for
 * every supported target, so the compiler atomics are available here. A
 * plain acquire load matters: the Windows Interlocked* alternative is a
 * full-barrier read-modify-write, and the event fast path is exactly the
 * place where the waiter wants a load, not an RMW. */
int pli_plat_load_i32_acquire(const void *p) {
  return __atomic_load_n((const int *)p, __ATOMIC_ACQUIRE);
}
void pli_plat_store_i32_release(void *p, int v) {
  __atomic_store_n((int *)p, v, __ATOMIC_RELEASE);
}
