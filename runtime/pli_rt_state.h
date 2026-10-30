/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* pli_rt_state.h — shared TU state for the split pli_runtime sources.
 *
 * Each rt_*.c includes this header (via pli_rt.h) so the cross-file users
 * keep working exactly as in the single-file layout: put_raw/next_char for
 * stream sinks/sources, the FILE routing table, STRING buffers, and the
 * data-directed input flag. One TU owns each definition (rt_stream.c);
 * every other TU sees it as extern.
 */
#ifndef PLI_RT_STATE_H
#define PLI_RT_STATE_H

#include <stddef.h>
#include <stdio.h>

#include "pli_wide128.h" /* PliI128 for the wide DECIMAL helpers below */
#include "pli_rt_plat.h" /* platform seam: every TU gets the pli_plat_* and
                           * threading declarations, so rt_*.c files include
                           * no system headers of their own. */
#include "rt_sync.h"     /* the runtime's lock layer (ADR-234): the tasking
                           * and stream code lock through rt_lock/rt_cond. */

/* ---------------------------------------------------------------------------
 * Ported libc primitives (runtime-reduction plan, Phases 1 & 4).
 *
 * Byte-oriented string/memory helpers and ASCII-only character
 * classification, ported from musl semantics so the runtime no longer needs
 * <string.h> / <ctype.h>. Each is `static inline` so every TU gets its own
 * copy and no symbol is exported (no ABI surface, no clash with user code).
 * Names are prefixed `pli_` to avoid colliding with libc.
 * ------------------------------------------------------------------------- */

static inline size_t pli_strlen(const char *s) {
  const char *a = s;
  for (; *a; ++a)
    ;
  return (size_t)(a - s);
}

static inline int pli_strncmp(const char *a, const char *b, size_t n) {
  for (; n && *a && *a == *b; ++a, ++b, --n)
    ;
  return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

static inline char *pli_strchr(const char *s, int c) {
  char ch = (char)c;
  for (; *s && *s != ch; ++s)
    ;
  return *s == ch ? (char *)s : NULL;
}

static inline void *pli_memcpy(void *d, const void *s, size_t n) {
  char *a = (char *)d;
  const char *b = (const char *)s;
  for (; n; --n)
    *a++ = *b++;
  return d;
}

static inline void *pli_memmove(void *d, const void *s, size_t n) {
  char *a = (char *)d;
  const char *b = (const char *)s;
  if (a == b)
    return d;
  if (a < b) {
    for (; n; --n)
      *a++ = *b++;
  } else {
    char *e = a + n;
    b += n;
    for (; n; --n)
      *--e = *--b;
  }
  return d;
}

static inline void *pli_memset(void *d, int c, size_t n) {
  unsigned char *a = (unsigned char *)d;
  unsigned char ch = (unsigned char)c;
  for (; n; --n)
    *a++ = ch;
  return d;
}

static inline int pli_memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  for (; n; --n, ++x, ++y)
    if (*x != *y)
      return *x < *y ? -1 : 1;
  return 0;
}

static inline void *pli_memchr(const void *s, int c, size_t n) {
  const unsigned char *a = (const unsigned char *)s;
  unsigned char ch = (unsigned char)c;
  for (; n; --n, ++a)
    if (*a == ch)
      return (void *)a;
  return NULL;
}

/* ASCII-only character classification (no locale; PL/I fixed collation). */
static inline int pli_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int pli_isalpha(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static inline int pli_isalnum(int c) {
  return pli_isdigit(c) || pli_isalpha(c);
}
static inline int pli_toupper(int c) {
  return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

/* Forward declarations for internal numeric helpers (defined in rt_get.c). */
long long pli_strtoll(const char *nptr, char **endptr, int base);
double pli_strtod(const char *nptr, char **endptr);

/* Forward declarations for the internal minimal formatter (rt_stream.c). */
int pli_snprintf(char *buf, size_t cap, const char *fmt, ...);

/* SYSPRINT state. A full implementation tracks page/line/column against
 * LINESIZE and PAGESIZE and raises ENDPAGE; M0 tracks the column only. */
extern int rt_col;
extern int rt_in_col;
extern int rt_items_on_line;
/* Data-directed output (rule (106), QR1.5): names emitted in the open DATA
 * list, and a flag suppressing the value's blank separator after NAME=. */
extern int rt_data_items;
extern int rt_data_value_next;
/* Set when get_token terminates at ';' (data-directed pairs, rule (106)),
 * so pli_get_data_next ends the list there. */
extern int rt_tok_semi;

/* STRING (rule 105) sink/source: when out_buf is non-null, list-directed output
 * is written into it instead of stdout; when in_buf is non-null, list-directed
 * input is read from it instead of stdin. */
extern char *rt_out_buf;
extern size_t rt_out_cap, rt_out_len;
extern const char *rt_in_buf;
extern size_t rt_in_len, rt_in_pos;

/* Named files (rules 100-103, 105): a fixed table indexed by the compile-time
 * slot assigned to each FILE variable. out_f/in_f are the streams selected by
 * the FILE ( f ) option of a PUT/GET; when set they override stdout/stdin. */
#define PLI_MAX_FILES 16
extern FILE *rt_pli_files[PLI_MAX_FILES];
 extern FILE *rt_out_f;
extern FILE *rt_in_f;
/* NONPRINT discard flag (ADR-204): when set, output goes nowhere instead of stdout. */
extern int rt_out_discard;
/* NONPRINT flag per file slot (ADR-204): when set, output to that file is discarded. */
extern int rt_pli_nonprint[PLI_MAX_FILES];
/* Page counters per file slot (PLI PAGENO pseudo-variable). */
extern volatile long long rt_pli_pages[PLI_MAX_FILES];

/* Stream helpers shared across TUs (defined in rt_stream.c). */
/* rt_put_raw writes bytes into the active sink. The caller holds the output
 * lock (rt_out_lock): every public output entry point brackets its writes,
 * so the stdout slab and the line state stay consistent when a task writes
 * concurrently with the main thread (ADR-233). */
void rt_put_raw(const char *p, size_t n);
int rt_next_char(void);
/* The input broker (ADR-238): the next byte of standard input, or EOF.
 * The read happens on the broker's thread, so a task that waits for input
 * parks instead of holding the worker it runs on. */
int rt_input_getc(void);
/* File-read broker (C2, ADR-298): fread under the fiber model parks the
 * task while a helper thread does the blocking read. */
size_t rt_fbroker_fread(void *buf, size_t n, FILE *f);
/* Wait for legacy OS-model tasks before the process exits (ADR-298). */
void rt_wait_tasks(void);
/* Select the input stream (WS-H4, ADR-272). */
void rt_select_input(FILE *f);
/* ADR-220: flush the stdout write slab before program exit or FILE routing. */
void rt_flush_stdout(void);
/* WS-H5: flush the file write slab at the end of a PUT, before routing or
 * closing the file it belongs to. */
void rt_flush_file_out(void);
void rt_file_out_detach(FILE *f);

/* ---------------------------------------------------------------------------
 * Stream serialisation (ADR-233).
 *
 * A CALL ... TASK (rule (79)) runs its callee on another thread, and the
 * stream state is process-global: two threads writing at once interleave their
 * bytes in the stdout slab and corrupt each other's lines, and two threads
 * reading at once split each other's records. The fix is one recursive mutex
 * per direction around the stream path - taken only once the program has a
 * task, so a program that never spawns one keeps the same code path (one
 * predictable branch per access, no lock).
 *
 * rt_tasks_active is 0 until the first task spawn; the flag is written exactly
 * once (inside the one-time init) and read after a pthread_create, so the
 * transition carries no data race. It is volatile so the optimiser cannot
 * cache the load across a loop.
 * ------------------------------------------------------------------------- */
extern rt_lock rt_out_mu;
extern rt_lock rt_in_mu;
extern volatile int rt_tasks_active;

/* Take/release the output lock. Recursive: an item writer calls the byte
 * writer, which locks on its own. */
static inline void rt_out_lock(void) {
  if (rt_tasks_active)
    rt_lock_acquire(&rt_out_mu);
}
static inline void rt_out_unlock(void) {
  if (rt_tasks_active)
    rt_lock_release(&rt_out_mu);
}
/* Take/release the input lock. Recursive, like the output lock: a token
 * reader is called from the record reader that already holds it. */
static inline void rt_in_lock(void) {
  if (rt_tasks_active)
    rt_lock_acquire(&rt_in_mu);
}
static inline void rt_in_unlock(void) {
  if (rt_tasks_active)
    rt_lock_release(&rt_in_mu);
}
/* Called by the task spawn before the thread is created: from then on every
 * stream operation runs under the matching lock. */
void rt_stream_enable_threads(void);

/* ---------------------------------------------------------------------------
 * Record READ serialisation (DEFERRED-WORK C4, ADR-342).
 *
 * A record READ does ftell, then hands the blocking fread to the file broker,
 * then stores the record position. Two tasks interleaving those steps would
 * both read the same record and leave the position wrong, and the broker has
 * one work slot. One fiber-aware lock makes the whole read a unit; a second
 * record read parks instead of blocking its worker. Off until the first task
 * spawn, like the stream locks above.
 * ------------------------------------------------------------------------- */
extern rt_lock rt_rec_read_mu;
static inline void rt_rec_read_lock(void) {
  if (rt_tasks_active)
    rt_lock_acquire(&rt_rec_read_mu);
}
static inline void rt_rec_read_unlock(void) {
  if (rt_tasks_active)
    rt_lock_release(&rt_rec_read_mu);
}

/* Compatibility aliases so split sources keep their original identifiers. */
#define col rt_col
#define items_on_line rt_items_on_line
#define data_items rt_data_items
#define data_value_next rt_data_value_next
#define tok_semi rt_tok_semi
#define out_buf rt_out_buf
#define out_cap rt_out_cap
#define out_len rt_out_len
#define in_buf rt_in_buf
#define in_len rt_in_len
#define in_pos rt_in_pos
#define in_col rt_in_col
#define pli_files rt_pli_files
#define out_f rt_out_f
#define in_f rt_in_f
#define put_raw rt_put_raw
#define next_char rt_next_char

/* Intra-runtime helpers shared across TUs (single definitions named). */
void rt_separate(void);
void rt_display_begin(void);
void rt_display_end(void);
void rt_format_decfixed(char *buf, size_t cap, long long v, long long q);
void rt_format_decfixed128(char *buf, size_t cap, PliI128 v, long long q);
int rt_get_token(char *buf, size_t cap);
int rt_data_namechar(int c);
void rt_pli_on_push(long long key, long long id, void *fn, void *ctx);
static inline long long rt_pli_on_top(long long key);
static inline void *rt_pli_on_top_fn(long long key);
static inline void *rt_pli_on_top_ctx(long long key);
void rt_pli_on_pop(long long key);
void pli_on_establish(long long key, long long id, char *fn, char *ctx,
                      long long base);
void pli_on_revert(long long key, long long base);

/* Centralised abort/exit helpers (Phase 6): pli_abort flushes the runtime
 * output, prints msg to stderr and exits 8; pli_exit exits with a code. */
void pli_abort(const char *msg);
void pli_exit(int code);

#define separate rt_separate
#define display_begin rt_display_begin
#define display_end rt_display_end
#define format_decfixed rt_format_decfixed
#define format_decfixed128 rt_format_decfixed128
#define get_token rt_get_token
#define data_namechar rt_data_namechar
#define pli_on_push rt_pli_on_push
#define pli_on_top rt_pli_on_top
#define pli_on_pop rt_pli_on_pop

#endif /* PLI_RT_STATE_H */
