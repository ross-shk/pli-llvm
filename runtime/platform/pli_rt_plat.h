/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* runtime/pli_rt_plat.h — the platform seam: everything the portable runtime
 * imports from the host. Successor to pli_rt_port.h (ADR-221): where the port
 * header hid each platform branch behind a static inline, this header declares
 * the same surface as real (out-of-line) symbols, so the platform TU can be
 * swapped at link time. Three groups:
 * - pli_plat_ prefixed functions: libc entry points the portable TUs may not
 *   name directly (allocation, FILE I/O, process spawn, calendar time, and
 *   the exit/abort raw ends used by the portable pli_exit/pli_abort);
 * - the threading backend (same API and names as before, now out-of-line;
 *   single-threaded custom platforms implement mutex/cond as no-ops);
 * - pli_alloc/pli_free/pli_exit/pli_abort stay portable PL/I ABI entry
 *   points (signatures in pli_rt_abi.def); only their host raw ends live
 *   here as pli_plat_free/pli_plat_exit/pli_plat_abort.
 * Layering rule: a platform TU must be self-contained - it may name only libc
 * and the symbols declared here, never portable-blob internals (portable code
 * always calls down; platform code never calls up). A custom platform (see
 * the --rt-platform driver flag) implements every declaration in this file;
 * anything it leaves undefined is diagnosed at compile time by name. */
#ifndef PLI_RT_PLAT_H
#define PLI_RT_PLAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h> /* uintptr_t for pli_thread (Windows backend) */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* --- allocation (custom platforms: any heap implementation; must tolerate free(NULL)) */
void *pli_plat_malloc(size_t n);
void *pli_plat_calloc(size_t n, size_t size);
void *pli_plat_realloc(void *p, size_t n);
void pli_plat_free(void *p);

/* --- process exit/abort raw ends (portable pli_exit/pli_abort flush first) */
void pli_plat_exit(int code);
void pli_plat_abort(void);

/* Spin-loop pause hint (moved out of the portable TUs). */
void pli_plat_pause(void);

/* --- process spawn (custom platforms without a shell: return NULL / -1) */
FILE *pli_plat_popen(const char *cmd, const char *mode);
int pli_plat_pclose(FILE *fp);

/* --- calendar time (custom platforms: any clock; NULL-safe) */
time_t pli_plat_time(time_t *t);
void pli_plat_localtime(const time_t *t, struct tm *out);

/* --- FILE I/O (custom platforms: any FILE-compatible layer, or stubs that
 * fail closed; stream files already choose "r"/"w", record files "rb"/"wb") */
FILE *pli_plat_fopen(const char *path, const char *mode);
int pli_plat_fclose(FILE *fp);
size_t pli_plat_fread(void *p, size_t size, size_t n, FILE *fp);
size_t pli_plat_fwrite(const void *p, size_t size, size_t n, FILE *fp);
int pli_plat_fgetc(FILE *fp);
int pli_plat_getchar(void);
int pli_plat_fputc(int c, FILE *fp);
int pli_plat_fflush(FILE *fp);
int pli_plat_ferror(FILE *fp);
/* Buffering control (for BUFFERED/UNBUFFERED OPEN options). */
int pli_plat_setvbuf(FILE *fp, char *buf, int mode, size_t size);
/* Sequential record positioning (REWRITE/DELETE, rule 112): a seek/tell pair
 * plus truncation after a DELETE shifts the tail down. */
int pli_plat_fseek(FILE *fp, long long off, int whence);
long long pli_plat_ftell(FILE *fp);
int pli_plat_ftruncate(FILE *fp, long long size);
int pli_plat_fprintf(FILE *fp, const char *fmt, ...);

/* --- the tasking model (ADR-234)
 *
 * The fiber scheduler (runtime/rt_sched.c) needs four things the portable
 * blob may not name itself: the environment (the PLI_TASK_* knobs), the
 * machine size (the worker count), a monotonic clock (the DELAY timer
 * heap), and a lazily committed stack region with a guard page below it
 * (a fiber stack - the pages are demand-paged, so the reservation costs
 * address space, not memory, and the stack grows into it without ever
 * moving).
 *
 * A fiber stack is allocated as `size` usable bytes plus one guard page
 * below; the returned base is page-aligned and the guard sits in
 * [base - pli_plat_page_size(), base). A stack that runs off its end
 * therefore lands on an inaccessible page instead of a neighbouring
 * allocation. */
const char *pli_plat_getenv(const char *name); /* "" when unset */
int pli_plat_cpu_count(void);                  /* >= 1 */
long long pli_plat_now_ms(void);               /* monotonic milliseconds */
long long pli_plat_now_us(void);               /* monotonic microseconds */
size_t pli_plat_page_size(void);               /* the guard page size */
/* Reserve a fiber stack: `*size_inout` usable bytes, lazily committed, with
 * one guard page below. The platform rounds the size up (to its page or its
 * chunk) and writes the size the caller actually got back to *size_inout:
 * the stack's top must be the top of that region, or the guard is not
 * where the fault handler expects it. Returns 0 on success and writes the
 * usable base to *out_base (page-aligned). */
int pli_plat_stack_alloc(size_t *size_inout, void **out_base);
/* Release a stack obtained from pli_plat_stack_alloc (same base and size). */
void pli_plat_stack_free(void *base, size_t size);
/* Read up to `n` bytes of standard input, blocking until at least one byte
 * is there or the input ends. Returns the number of bytes read, 0 at the
 * end of the input, -1 on error. The input broker (ADR-238) calls this on
 * its own thread, so a program that reads stdin does not block a worker. */
int pli_plat_stdin_read(char *buf, int n);
/* Prepare the calling thread for the tasking runtime: the scheduler calls
 * it on every thread it runs tasks on, before the first task. Windows has
 * nothing to do (its fault handler is process-wide); POSIX installs the
 * stack-overflow signal handler and the *thread's* alternate stack, which
 * the handler needs to report an overflow at all. */
void pli_plat_thread_init(void);

/* --- threading backend (one backend per platform TU; single-threaded custom
 * platforms: mutex/cond as no-ops over a non-shared state, spawn as
 * synchronous call or an error return — see the contract on each function) */
/* Platform-specific backend types: pulled in from the per-OS header so the
 * portable seam stays branch-free at the top level. */
#if defined(_WIN32)
#include "windows/pli_rt_plat.h"
#else
#include "posix/pli_rt_plat.h"
#endif

/* Mutual exclusion around the shared EVENT/task table. */
void pli_mutex_init(pli_mutex *m);
void pli_mutex_lock(pli_mutex *m);
void pli_mutex_unlock(pli_mutex *m);
/* A recursive mutex: the same thread may take it twice. The output
 * serialiser (ADR-233) nests its locks - an item writer calls the byte
 * writer, which locks on its own - so it needs this, not pli_mutex_init.
 * Single-threaded custom platforms: a no-op here, as for pli_mutex_init. */
void pli_mutex_init_recursive(pli_mutex *m);
/* Condition broadcast/wait paired with the mutex above. */
void pli_cond_init(pli_cond *c);
void pli_cond_wait(pli_cond *c, pli_mutex *m);
/* Wait with a timeout in milliseconds: returns 1 when the condition was
 * signalled, 0 on timeout. A negative timeout waits forever — the scheduler
 * parks an idle worker on it, so the worker can re-check the DELAY timers
 * without a dedicated timer thread. */
int pli_cond_timedwait(pli_cond *c, pli_mutex *m, long long timeout_ms);
void pli_cond_signal(pli_cond *c);
void pli_cond_broadcast(pli_cond *c);
/* One-time initialisation for mutexes/conds with no static initializer. */
void pli_call_once(pli_once *o, void (*fn)(void));
/* Atomic 32-bit EVENTS (QR2.8). An EVENT is a PL/I variable (an i32 word),
 * so it cannot be declared _Atomic; the portable runtime reaches it only
 * through these. The completing task stores with release and a waiter loads
 * with acquire, so a waiter that sees the flag also sees the task's writes.
 * A custom platform must provide both. */
int pli_plat_load_i32_acquire(const void *p);
void pli_plat_store_i32_release(void *p, int v);
/* Spawn a thread running fn(arg); returns 0 on success. */
int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg);
void pli_thread_detach(pli_thread th);
/* Spawn a detached thread running fn(arg); returns 0 on success. */
int pli_spawn_detached(void *(*fn)(void *), void *arg);
/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
void pli_sleep_ms(long long ms);

#endif /* PLI_RT_PLAT_H */
