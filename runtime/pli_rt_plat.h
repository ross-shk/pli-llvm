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

/* --- process spawn (custom platforms without a shell: return NULL / -1) */
FILE *pli_plat_popen(const char *cmd, const char *mode);
int pli_plat_pclose(FILE *fp);

/* --- calendar time (custom platforms: fill *out from any clock; NULL-safe) */
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
int pli_plat_fprintf(FILE *fp, const char *fmt, ...);

/* --- threading backend (one backend per platform TU; single-threaded custom
 * platforms: mutex/cond as no-ops over a non-shared state, spawn as
 * synchronous call or an error return — see the contract on each function) */
#if defined(_WIN32)
/* Windows backend types (single selection block, first half).
 * Keep the system header out of macro pollution: pli_rt.h defines
 * VOID/I64/... while expanding pli_rt_abi.def, and windows.h (winnt.h)
 * collapses with 'unknown type name CHAR/LONG/...' when VOID is a macro.
 * pli_rt.h now includes this header outside that macro region, but guard
 * here too so direct/odd-order inclusion stays safe. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* push_macro works even when the name is currently undefined (pop restores
 * that state) on MSVC/clang, so push/undef/pop unconditionally. */
#pragma push_macro("VOID")
#pragma push_macro("I64")
#pragma push_macro("I32")
#pragma push_macro("I8")
#pragma push_macro("DOUBLE")
#pragma push_macro("PTR")
#pragma push_macro("CPTR")
#pragma push_macro("IPTR")
#pragma push_macro("PLI_FN")
#undef VOID
#undef I64
#undef I32
#undef I8
#undef DOUBLE
#undef PTR
#undef CPTR
#undef IPTR
#undef PLI_FN
#include <windows.h>
#pragma pop_macro("PLI_FN")
#pragma pop_macro("IPTR")
#pragma pop_macro("CPTR")
#pragma pop_macro("PTR")
#pragma pop_macro("DOUBLE")
#pragma pop_macro("I8")
#pragma pop_macro("I32")
#pragma pop_macro("I64")
#pragma pop_macro("VOID")
typedef CRITICAL_SECTION pli_mutex;
typedef CONDITION_VARIABLE pli_cond;
typedef INIT_ONCE pli_once;
typedef uintptr_t pli_thread;
#define PLI_ONCE_INIT INIT_ONCE_STATIC_INIT
#else
/* POSIX backend types (single selection block, second half). */
#include <pthread.h>
typedef pthread_mutex_t pli_mutex;
typedef pthread_cond_t pli_cond;
typedef pthread_once_t pli_once;
typedef pthread_t pli_thread;
#define PLI_ONCE_INIT PTHREAD_ONCE_INIT
#endif

/* Mutual exclusion around the shared EVENT/task table. */
void pli_mutex_init(pli_mutex *m);
void pli_mutex_lock(pli_mutex *m);
void pli_mutex_unlock(pli_mutex *m);
/* Condition broadcast/wait paired with the mutex above. */
void pli_cond_init(pli_cond *c);
void pli_cond_wait(pli_cond *c, pli_mutex *m);
void pli_cond_signal(pli_cond *c);
void pli_cond_broadcast(pli_cond *c);
/* One-time initialisation for mutexes/conds with no static initializer. */
void pli_call_once(pli_once *o, void (*fn)(void));
/* Spawn a thread running fn(arg); returns 0 on success. */
int pli_spawn(pli_thread *th, void *(*fn)(void *), void *arg);
void pli_thread_detach(pli_thread th);
/* Spawn a detached thread running fn(arg); returns 0 on success. */
int pli_spawn_detached(void *(*fn)(void *), void *arg);
/* Sleep for whole milliseconds; ms <= 0 is a no-op. */
void pli_sleep_ms(long long ms);

#endif /* PLI_RT_PLAT_H */
