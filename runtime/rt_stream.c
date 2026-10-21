/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_stream.c — PL/I runtime library (pli_runtime): stream output: SKIP/PAGE, DISPLAY, PUT LIST/PUT DATA. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* Buffered output routes through pli_plat_fwrite/pli_plat_fgetc/pli_plat_getchar
 * (the platform seam). The fputc_ helper further down is the internal
 * formatter sink, unrelated to stdio fputc. */

/* SYSPRINT state. A full implementation tracks page/line/column against
 * LINESIZE and PAGESIZE and raises ENDPAGE; M0 tracks the column only.
 *
 * The stream state below is process-global, not thread-local: a CALL ...
 * TASK (rule (79)) runs its callee on another thread, and a concurrent PUT
 * from two threads can interleave in the stdout slab. The output serialiser
 * below (ADR-233) fixes that with one recursive mutex around the output
 * path. Making the state thread-local would fix it too, but on Mach-O a
 * thread-local access is a descriptor load plus an indirect call to the TLS
 * resolver, and the resolver call lands in the output path of every PUT:
 * measured on benchmarks/io_throughput.pli it costs +69% (20.0 ms -> 33.9 ms
 * for 200k PUTs), and the cost is per function that touches the state, not
 * per variable, so grouping the state into one structure does not help. */
int rt_col = 0;
int rt_in_col = 0;
int rt_items_on_line = 0;
/* Data-directed output (rule (106), QR1.5): names emitted in the open DATA
 * list, and a flag suppressing the value's blank separator after NAME=. */
int rt_data_items = 0;
int rt_data_value_next = 0;
/* Set when get_token terminates at ';' (data-directed pairs, rule (106)),
 * so pli_get_data_next ends the list there. */
int rt_tok_semi = 0;

/* STRING (rule 105) sink/source: when rt_out_buf is non-null, list-directed output
 * is written into it instead of stdout; when rt_in_buf is non-null, list-directed
 * input is read from it instead of stdin. */
char *rt_out_buf = NULL;
size_t rt_out_cap = 0, rt_out_len = 0;
const char *rt_in_buf = NULL;
size_t rt_in_len = 0, rt_in_pos = 0;

/* Named files (rules 100-103, 105): a fixed table indexed by the compile-time
 * slot assigned to each FILE variable. rt_out_f/rt_in_f are the streams selected by
 * the FILE ( f ) option of a PUT/GET; when set they override stdout/stdin. */
#define PLI_MAX_FILES 16
FILE *rt_pli_files[PLI_MAX_FILES] = {0};
int rt_pli_nonprint[PLI_MAX_FILES] = {0};
/* Volatile: the compiler must emit a real memory access for every read/write
   so the optimizer cannot see through the pli_set_pageno/pli_pageno wrappers
   and eliminate them as dead stores/loads when bitcode is merged at -O2. */
volatile long long rt_pli_pages[PLI_MAX_FILES] = {0};
FILE *rt_out_f = NULL;
FILE *rt_in_f = NULL;
/* NONPRINT flag (ADR-204): when rt_out_discard is set, output is discarded
   instead of falling through to stdout. Set by pli_put_select on a NONPRINT file. */
int rt_out_discard = 0;

/* Stdout write buffer (ADR-220): tiny fwrite calls dominate IO-throughput
 * (benchmarks/io_throughput.pli). Accumulate stdout writes in an 8 KiB slab
 * and drain with one fwrite per fill; flush the remainder at rt_fini. */
#define RT_STDOUT_BUF 8192
static char rt_stdout_buf[RT_STDOUT_BUF];
static size_t rt_stdout_pos = 0;

/* File input buffer (WS-H4, ADR-272): a list-directed GET reads a record one
 * character at a time, and a locked stdio call per character dominated the
 * file read path — measured on benchmarks/ir/file_io.pli, the same 500k
 * records take 340 ms through the per-character path and 30 ms through the
 * buffered one. The characters are therefore pulled from a slab that one
 * fread fills.
 *
 * The slab belongs to the FILE it was filled from, not to the currently
 * selected stream: a GET statement unselects the file when it ends, and the
 * characters already read but not yet consumed must survive that. Two tasks
 * reading the same file would otherwise lose the second half of a record
 * between them. Nothing in the runtime seeks a stream, so a slab cannot go
 * stale behind the program's back; a different file starts a fresh slab. */
#define RT_FILE_IN_BUF 8192
static char rt_in_f_buf[RT_FILE_IN_BUF];
static size_t rt_in_f_len = 0;
static size_t rt_in_f_pos = 0;
static FILE *rt_in_f_owner = NULL; /* the file the slab was filled from */

/* ---------------------------------------------------------------------------
 * Stream serialisation (ADR-233).
 *
 * One recursive mutex per direction guards the stream path once the program
 * has a task. The mutexes are recursive because the item writers call the byte
 * writer (rt_put_raw), which is itself an internal helper under the caller's
 * lock.
 *
 * The serialiser stays off until the first task spawn: a program that never
 * spawns a task pays one predictable branch per access and no lock at all.
 * The flag is written exactly once, inside the one-time init, so no two
 * threads ever write it; the task threads read it after their pthread_create,
 * which orders the read after the write.
 * ------------------------------------------------------------------------- */
pli_mutex rt_out_mu;
pli_mutex rt_in_mu;
static pli_once rt_out_once = PLI_ONCE_INIT;
volatile int rt_tasks_active = 0;

/* One-time init: the mutexes first, then the flag that turns the serialiser
 * on (a lock attempt must never find an uninitialised mutex). */
static void rt_out_init(void) {
  pli_mutex_init_recursive(&rt_out_mu);
  pli_mutex_init_recursive(&rt_in_mu);
  rt_tasks_active = 1;
}

/* Called by the task spawn before the thread is created. */
void rt_stream_enable_threads(void) { pli_call_once(&rt_out_once, rt_out_init); }

/* One output statement under the lock (ADR-233): the compiler brackets a PUT
 * or DISPLAY statement with these, so the whole record reaches the stream
 * before another task's record does. No-ops until the program has a task. */
void pli_out_begin(void) { rt_out_lock(); }
void pli_out_end(void) { rt_out_unlock(); }

/* One input statement under the lock (ADR-233): the compiler brackets a GET
 * statement with these, so a task reads whole records. The input lock is
 * separate from the output one: a task waiting for input must not hold off
 * another task's output. */
void pli_in_begin(void) { rt_in_lock(); }
void pli_in_end(void) { rt_in_unlock(); }

void rt_flush_stdout(void) {
  rt_out_lock();
  if (rt_stdout_pos > 0) {
    pli_plat_fwrite(rt_stdout_buf, 1, rt_stdout_pos, stdout);
    rt_stdout_pos = 0;
  }
  rt_out_unlock();
}

/* The byte writer. The caller holds the output lock (rt_out_lock): every
 * public output entry point brackets its writes, so the slab, the column and
 * the item count stay consistent when a task writes concurrently. */
void rt_put_raw(const char *p, size_t n) {
  if (rt_out_f) {
    pli_plat_fwrite(p, 1, n, rt_out_f);
    rt_col += (int)n;
    return;
  }
  if (rt_out_discard)
    return;
  if (rt_out_buf) {
    size_t take = n < (rt_out_cap - rt_out_len) ? n : (rt_out_cap - rt_out_len);
    if (take > 0)
      pli_memmove(rt_out_buf + rt_out_len, p, take);
    rt_out_len += take;
    return;
  }
  /* Buffered stdout: copy into the slab when it fits, else flush + drain. */
  if (rt_stdout_pos + n <= RT_STDOUT_BUF) {
    pli_memcpy(rt_stdout_buf + rt_stdout_pos, p, n);
    rt_stdout_pos += n;
  } else {
    rt_flush_stdout();
    pli_plat_fwrite(p, 1, n, stdout);
  }
  rt_col += (int)n;
}

/* Select the input stream. The slab is not touched here: it belongs to the
 * file it was filled from, and a GET statement unselects the file between
 * statements (the characters read but not yet consumed must survive that). */
void rt_select_input(FILE *f) { rt_in_f = f; }

/* Read one input character from the active FILE stream, STRING source, or
 * stdin. Tracks the input column for COLUMN positioning on GET EDIT. */
int rt_next_char(void) {
  int c;
  if (rt_in_f) {
    if (rt_in_f_owner != rt_in_f) {
      /* a different file: its slab starts afresh */
      rt_in_f_owner = rt_in_f;
      rt_in_f_len = 0;
      rt_in_f_pos = 0;
    }
    if (rt_in_f_pos >= rt_in_f_len) {
      rt_in_f_len = pli_plat_fread(rt_in_f_buf, 1, RT_FILE_IN_BUF, rt_in_f);
      rt_in_f_pos = 0;
      if (rt_in_f_len == 0)
        return EOF;
    }
    c = (unsigned char)rt_in_f_buf[rt_in_f_pos++];
  } else if (rt_in_buf)
    c = rt_in_pos < rt_in_len ? (unsigned char)rt_in_buf[rt_in_pos++] : EOF;
  else
    c = pli_plat_getchar();
  if (c == EOF)
    return c;
  if (c == '\n')
    rt_in_col = 0;
  else
    ++rt_in_col;
  return c;
}

void pli_put_skip(long long n) {
  rt_out_lock();
  if (n < 1) n = 1;
  if (rt_out_f) {
    for (long long i = 0; i < n; ++i) rt_put_raw("\n", 1);
    rt_col = 0;
    rt_items_on_line = 0;
    rt_out_unlock();
    return;
  }
  if (rt_col > 0) {
    rt_put_raw("\n", 1);
    rt_col = 0;
  }
  for (long long i = 1; i < n; ++i) rt_put_raw("\n", 1);
  rt_items_on_line = 0;
  rt_out_unlock();
}

void pli_put_page(void) {
  rt_out_lock();
  if (rt_col > 0) rt_put_raw("\n", 1);
  rt_put_raw("\f", 1);
  rt_col = 0;
  rt_items_on_line = 0;
  rt_out_unlock();
}

/* List-directed output separates successive items by a blank; character
 * values are written without enclosing quotation marks. After a DATA name
 * the value follows its '=' directly, with no blank.
 * The caller holds the output lock (ADR-233). */
void rt_separate(void) {
  if (rt_data_value_next) {
    rt_data_value_next = 0;
    rt_items_on_line++;
    return;
  }
  if (rt_items_on_line > 0) rt_put_raw(" ", 1);
  rt_items_on_line++;
}

void pli_put_list_char(const char *p, long long len) {
  rt_out_lock();
  separate();
  if (len > 0) rt_put_raw(p, (size_t)len);
  rt_out_unlock();
}

/* DISPLAY (rule 114, ADR-081): one scalar value plus a newline, with the
 * same value formats as list-directed output but no item separator.
 * rt_display_begin/rt_display_end run under the caller's output lock. */
void rt_display_end(void) {
  // The line is complete: reset the column and item count so fini and a
  // following PUT start clean.
  rt_put_raw("\n", 1);
  rt_col = 0;
  rt_items_on_line = 0;
}
/* A DISPLAY starts on a fresh line: end a pending PUT line first (as SKIP
 * does), so mixed PUT/DISPLAY output never joins two values on one line. */
void rt_display_begin(void) {
  if (rt_col > 0) {
    rt_put_raw("\n", 1);
    rt_col = 0;
  }
  rt_items_on_line = 0;
}
void pli_display_char(const char *p, long long len) {
  rt_out_lock();
  display_begin();
  if (len > 0) rt_put_raw(p, (size_t)len);
  display_end();
  rt_out_unlock();
}
void pli_display_fixed(long long v) {
  char buf[32];
  int n = pli_snprintf(buf, sizeof buf, "%lld", v);
  rt_out_lock();
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
  rt_out_unlock();
}
void pli_display_float(double v) {
  char buf[64];
  int n = pli_snprintf(buf, sizeof buf, "%.6g", v);
  rt_out_lock();
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
  rt_out_unlock();
}
void pli_display_bit(unsigned char b) {
  rt_out_lock();
  display_begin();
  rt_put_raw(b ? "1" : "0", 1);
  display_end();
  rt_out_unlock();
}
void pli_display_complex(double re, double im) {
  char buf[128];
  int n = pli_snprintf(buf, sizeof buf, "%.6g%+.6gI", re, im);
  rt_out_lock();
  display_begin();
  rt_put_raw(buf, (size_t)n);
  display_end();
  rt_out_unlock();
}

void pli_put_list_fixed(long long v) {
  char buf[32];
  int n = pli_snprintf(buf, sizeof buf, "%lld", v);
  rt_out_lock();
  separate();
  rt_put_raw(buf, (size_t)n);
  rt_out_unlock();
}

/* CHAR(fixed): decimal image of v, blank-padded to dstcap. */
void pli_char_of_fixed(char *dst, long long dstcap, long long v) {
  char tmp[24];
  int n = pli_snprintf(tmp, sizeof tmp, "%lld", v);
  long long i = 0;
  for (; i < n && i < dstcap; ++i)
    dst[i] = tmp[i];
  for (; i < dstcap; ++i)
    dst[i] = ' ';
}

/* CHAR(float): %.6g image (the same readable choice as PUT LIST output),
 * blank-padded to dstcap. */
void pli_char_of_float(char *dst, long long dstcap, double x) {
  char tmp[48];
  int n = pli_snprintf(tmp, sizeof tmp, "%.6g", x);
  long long i = 0;
  while (i < n && i < dstcap) {
    dst[i] = tmp[i];
    ++i;
  }
  for (; i < dstcap; ++i)
    dst[i] = ' ';
}

/* FIXED(char): parse decimal text, truncating any fraction toward zero.
 * Leading blanks, one optional sign, then digits; stops at the first
 * non-digit. Sets *ok = 1 when at least one digit was consumed, 0 when the
 * text has no numeric value (CONVERSION condition, rule 94, ADR-170). */
long long pli_fixed_of_char(const char *s, long long slen, int *ok) {
  long long i = 0;
  while (i < slen && (s[i] == ' ' || s[i] == '\t'))
    ++i;
  int neg = 0;
  if (i < slen && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    ++i;
  }
  long long v = 0;
  int sawDigit = 0;
  int overflow = 0;
  while (i < slen && s[i] >= '0' && s[i] <= '9') {
    unsigned d = (unsigned)(s[i] - '0');
    /* The accumulator overflows past 19 digits: stop and report it, rather
     * than wrapping silently (signed overflow is undefined). */
    if (!overflow && (v > (0x7FFFFFFFFFFFFFFFLL - (long long)d) / 10)) {
      overflow = 1;
      v = 0x7FFFFFFFFFFFFFFFLL;
    } else if (!overflow) {
      v = v * 10 + (long long)d;
    }
    ++i;
    sawDigit = 1;
  }
  *ok = sawDigit && !overflow;
  return neg ? -v : v;
}

/* FIXED(float): truncate toward zero (NaN reads as 0, out-of-range clamps;
 * SIZE routing for the clamping case is a follow-up). */
long long pli_fixed_of_float(double x) {
  if (x != x)
    return 0;
  if (x >= 9223372036854775807.0)
    return 9223372036854775807LL;
  if (x < -9223372036854775808.0)
    return (-9223372036854775807LL - 1);
  return (long long)x;
}

/* Write the decimal digits of an unsigned magnitude into buf (at most 20
 * digits) and return the count. */
static size_t pli_fmt_u64(char *buf, unsigned long long v) {
  char tmp[20];
  int n = 0;
  if (v == 0)
    tmp[n++] = '0';
  while (v && n < (int)sizeof tmp) {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  for (int i = 0; i < n; ++i)
    buf[i] = tmp[n - 1 - i];
  return (size_t)n;
}

/* FIXED DECIMAL output (QR1.2): the stored integer holds value * 10^q, so
 * print exactly q fraction digits. The buffer is caller-sized: the number of
 * fraction digits is bounded by the room left, and 10^i is never formed past
 * i = 18 (10^19 does not fit an i64). */
void rt_format_decfixed(char *buf, size_t cap, long long v, long long q) {
  char *p = buf;
  size_t room = cap;
  /* Negate through an unsigned magnitude: -LLONG_MIN is undefined. */
  unsigned long long mag = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL
                                 : (unsigned long long)v;
  if (v < 0) {
    if (room < 2)
      goto done;
    *p++ = '-';
    --room;
  }
  {
    /* 10^18 is the largest power of ten an i64 holds; a wider scale cannot
     * be represented by the stored integer anyway. */
    long long fdigits = q < 18 ? q : 18;
    unsigned long long factor = 1;
    for (long long i = 0; i < fdigits; ++i)
      factor *= 10;
    char digits[24];
    size_t n = pli_fmt_u64(digits, mag / factor);
    for (size_t i = 0; i < n && room > 1; ++i) {
      *p++ = digits[i];
      --room;
    }
    if (q > 0 && room > 1) {
      *p++ = '.';
      --room;
      unsigned long long rem = mag % factor;
      /* Build the fraction digits least-significant first, then reverse. */
      char frac[20];
      long long ndig = q < (long long)sizeof frac ? q : (long long)sizeof frac;
      for (long long i = 0; i < ndig; ++i) {
        frac[i] = (char)('0' + (rem % 10));
        rem /= 10;
      }
      for (long long i = ndig - 1; i >= 0 && room > 1; --i) {
        *p++ = frac[i];
        --room;
      }
    }
  }
done:
  *p = '\0';
}
void pli_put_list_decfixed(long long v, long long q) {
  char buf[80];
  format_decfixed(buf, sizeof buf, v, q);
  rt_out_lock();
  separate();
  rt_put_raw(buf, pli_strlen(buf));
  rt_out_unlock();
}
void pli_display_decfixed(long long v, long long q) {
  char buf[80];
  format_decfixed(buf, sizeof buf, v, q);
  rt_out_lock();
  display_begin();
  rt_put_raw(buf, pli_strlen(buf));
  display_end();
  rt_out_unlock();
}

/* Print a 128-bit magnitude into buf (caller ensures room); returns count.
 * The stored integer of a >18 digit FIXED DECIMAL (ADR-191) overflows i64, so
 * output formats the full 128-bit value directly instead of truncating.
 * There is no printf length modifier for 128 bits, so digits come from
 * repeated div/mod by 10 through the portable helpers (MSVC lacks __int128;
 * clang-cl supports it natively).
 */
static size_t pli_fmt128(char *buf, size_t cap, PliU128 v) {
  char tmp[40];
  int n = 0;
  if (pli_iszero_u(v))
    tmp[n++] = '0';
  while (!pli_iszero_u(v) && n < (int)sizeof(tmp))
    tmp[n++] = (char)('0' + pli_divmod10(&v));
  if ((size_t)n > cap)
    n = (int)cap;
  for (int i = 0; i < n; ++i)
    buf[i] = tmp[n - 1 - i];
  return (size_t)n;
}

/* >18 digit FIXED DECIMAL output (QR1.2): format a 128-bit stored integer with
 * exactly q fraction digits (round-trip faithful, no FP). */
void rt_format_decfixed128(char *buf, size_t cap, PliI128 v, long long q) {
  char *p = buf;
  PliU128 mag = pli_mag_s(v); /* INT128_MIN-safe magnitude */
  PliU128 factor = pli_u128(1u, 0u);
  PliU128 ten = pli_u128(10u, 0u);
  PliU128 ip, rem;
  long long i;
  if (pli_isneg_s(v))
    *p++ = '-';
  /* 10^q must fit the 128-bit factor: 38 digits is the limit. */
  for (i = 0; i < q && i < 38; ++i)
    factor = pli_mul_u(factor, ten);
  pli_divmod_u(mag, factor, &ip, &rem);
  p += pli_fmt128(p, cap - (size_t)(p - buf), ip);
  if (q > 0) {
    /* The fraction digits must fit the caller's buffer: the integer part
     * takes up to 39 digits and the '.' one more. */
    size_t room = cap - (size_t)(p - buf);
    if (room > 1) {
      char frac[40];
      *p++ = '.';
      --room;
      /* 10^q must fit the 128-bit factor: 38 digits is the limit. */
      long long ndig = q < 38 ? q : 38;
      if (ndig > (long long)sizeof frac)
        ndig = (long long)sizeof frac;
      for (i = 0; i < ndig; ++i)
        frac[i] = (char)('0' + pli_divmod10(&rem));
      for (i = ndig - 1; i >= 0 && room > 1; --i) {
        *p++ = frac[i];
        --room;
      }
    }
  }
  *p = '\0';
}
/* >18 digit FIXED DECIMAL output (QR1.2): format a 128-bit stored integer with
 * exactly q fraction digits (round-trip faithful, no FP). The value arrives
 * as (lo, hi) halves (see pli_rt_abi.def): a by-value 128-bit parameter
 * would lower differently on each target (indirect on Windows x64). */
void pli_put_list_decfixed128(long long lo, long long hi, long long q) {
  char buf[128];
  PliI128 v = pli_to_s(pli_u128((uint64_t)lo, (uint64_t)hi));
  rt_format_decfixed128(buf, sizeof buf, v, q);
  rt_out_lock();
  separate();
  rt_put_raw(buf, pli_strlen(buf));
  rt_out_unlock();
}
void pli_display_decfixed128(long long lo, long long hi, long long q) {
  char buf[128];
  PliI128 v = pli_to_s(pli_u128((uint64_t)lo, (uint64_t)hi));
  rt_format_decfixed128(buf, sizeof buf, v, q);
  rt_out_lock();
  display_begin();
  rt_put_raw(buf, pli_strlen(buf));
  display_end();
  rt_out_unlock();
}

/* r >> scale for the portable 128-bit type (scale < 64). */
#ifdef PLI_WIDE128_PORTABLE
static unsigned long long pli_shr128(PliU128 v, int scale) {
  if (scale >= 64)
    return 0;
  if (scale == 0)
    return v.lo;
  return (v.hi << (64 - scale)) | (v.lo >> scale);
}
#else
static unsigned long long pli_shr128(PliU128 v, int scale) {
  return scale >= 128 ? 0ULL : (unsigned long long)(v >> scale);
}
#endif

/* List-directed output of a scaled FIXED value (ADR-006): the stored integer v
 * holds the value * 2^scale, so print v / 2^scale exactly as a decimal. The
 * fractional part of a dyadic rational terminates, so the digit loop is exact. */
void pli_put_list_fixed_scaled(long long v, long long scale) {
  char buf[96];
  char *p = buf;
  if (scale <= 0) {  /* fall back to the plain formatter */
    pli_put_list_fixed(v);
    return;
  }
  /* 1LL << 63 is undefined; a binary scale above 62 cannot be represented
   * by the stored integer anyway. */
  if (scale > 62) scale = 62;
  if (v < 0) { *p++ = '-'; v = -v; }
  unsigned long long factor = 1ULL << (unsigned)scale;  /* 2^scale */
  int n = pli_snprintf(p, sizeof buf - (size_t)(p - buf), "%lld", v / (long long)factor);
  p += n;
  unsigned long long rem = (unsigned long long)v % factor;
  rt_out_lock();
  if (rem != 0) {
    *p++ = '.';
    char digit[64];
    int cnt = 0;
    /* The remainder is scaled by 10 in 128 bits: rem < 2^scale, and
     * rem * 10 exceeds 64 bits for a scale above 59. */
    while (rem != 0 && cnt < 60) {
      PliU128 r = pli_mul_u(pli_u128(rem, 0u), pli_u128(10u, 0u));
      digit[cnt++] = (char)(pli_shr128(r, (int)scale) & 9u);
#ifdef PLI_WIDE128_PORTABLE
      rem = r.lo & (factor - 1);
#else
      rem = (unsigned long long)r & (factor - 1);
#endif
    }
    for (int i = 0; i < cnt; ++i) *p++ = '0' + digit[i];
  }
  separate();
  rt_put_raw(buf, (size_t)(p - buf));
  rt_out_unlock();
}

void pli_put_list_float(double v) {
  char buf[64];
  /* PL/I would use E-format for FLOAT; %.6g keeps the wireframe readable. */
  int n = pli_snprintf(buf, sizeof buf, "%.6g", v);
  rt_out_lock();
  separate();
  rt_put_raw(buf, (size_t)n);
  rt_out_unlock();
}

void pli_put_list_bit(unsigned char b) {
  rt_out_lock();
  separate();
  rt_put_raw(b ? "1" : "0", 1);
  rt_out_unlock();
}

/* Complex output (CM5, ADR-085): real part, sign, imaginary magnitude, I. */
void pli_put_list_complex(double re, double im) {
  char buf[128];
  int n = pli_snprintf(buf, sizeof buf, "%.6g%+.6gI", re, im);
  rt_out_lock();
  separate();
  rt_put_raw(buf, (size_t)n);
  rt_out_unlock();
}

/* Data-directed output (rule (106), QR1.5): `NAME=` opens an item (", "
 * between items) and the value follows with no blank; `;` closes the list. */
void pli_put_data_name(const char *p, long long len) {
  rt_out_lock();
  if (rt_data_items > 0) rt_put_raw(", ", 2);
  if (len > 0) rt_put_raw(p, (size_t)len);
  rt_put_raw("=", 1);
  rt_data_items++;
  rt_data_value_next = 1;
  rt_out_unlock();
}

void pli_put_data_end(void) {
  rt_out_lock();
  rt_put_raw(";", 1);
  rt_items_on_line++;
  rt_data_items = 0;
  rt_data_value_next = 0;
  rt_out_unlock();
}

/* ---------------------------------------------------------------------------
 * Ported minimal formatter (runtime-reduction plan, Phase 3).
 *
 * A small snprintf supporting exactly the conversions the runtime emits:
 *   %d %lld            signed decimal (int / long long)
 *   %0*lld %04d %02d   zero-padded to a fixed width (numeric or '*')
 *   %f %.6f %.*f       fixed-point, optional precision
 *   %e %.*e            scientific (d.ddde±XX)
 *   %g %.6g %+.6g      general: %f/%e chosen by magnitude, trailing zeros
 *                      stripped; '+' flag for the complex-image sign
 *   %s %c              (unused, provided for completeness)
 * No %p/%n/%a/%u/%zu, no locale, no left/right space padding beyond '0'.
 * Returns the would-be length (snprintf semantics); writes at most cap-1
 * characters plus the NUL terminator.
 * ------------------------------------------------------------------------- */

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} Fmt;

static void fputc_(Fmt *f, char c) {
  if (f->len + 1 < f->cap)
    f->buf[f->len] = c;
  f->len++;
}

static void fputs_(Fmt *f, const char *s, size_t n) {
  for (size_t i = 0; i < n; ++i)
    fputc_(f, s[i]);
}

static void fpad_(Fmt *f, char c, size_t n) {
  while (n--)
    fputc_(f, c);
}

static void fmt_int(Fmt *f, long long v, int width, int zero) {
  char tmp[24];
  int n = 0;
  unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1
                               : (unsigned long long)v;
  do {
    tmp[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  int sign = v < 0;
  int pad = width - (n + sign);
  if (pad < 0)
    pad = 0;
  if (!zero)
    fpad_(f, ' ', (size_t)pad);
  if (sign)
    fputc_(f, '-');
  if (zero)
    fpad_(f, '0', (size_t)pad);
  while (n)
    fputc_(f, tmp[--n]);
}

static double pow10_i(int e) {
  double r = 1.0;
  if (e >= 0)
    for (int i = 0; i < e; ++i)
      r *= 10.0;
  else
    for (int i = 0; i < -e; ++i)
      r /= 10.0;
  return r;
}

/* True when v is an infinity (its magnitude exceeds DBL_MAX). */
static int is_inf(double v) {
  return v > 1.7976931348623157e308 || v < -1.7976931348623157e308;
}

/* Decimal exponent E of v >= 0 such that 1 <= v*10^-E < 10 (E = floor(log10 v),
 * 0 for v == 0). This is the %g/%e exponent. */
static int dec_exp(double v) {
  if (v == 0.0)
    return 0;
  /* An infinity never reaches 10^e: inf/10 is still inf, so the loop below
   * would spin forever. A NaN fails every comparison. */
  if (v != v || is_inf(v))
    return 0;
  int e = 0;
  if (v >= 1.0) {
    while (v >= 10.0) {
      v /= 10.0;
      ++e;
    }
  } else {
    while (v < 1.0) {
      v *= 10.0;
      --e;
    }
  }
  return e;
}

/* True when v is negative (including -0.0), without needing signbit(). */
static int is_neg(double v) {
  return v < 0.0 || (v == 0.0 && 1.0 / v < 0.0);
}

/* Print a non-negative v with p digits after the decimal point (no sign),
 * rounding half to even (printf semantics). The whole part is printed
 * directly (no overflow for large magnitudes); only the fraction in [0,1) is
 * scaled by 10^p, exact for p <= 18 (clamped). The half-way tie is broken to
 * even, where the retained last digit's parity must account for the whole
 * part when p == 0 (rounding to an integer). Avoids libm floor/ceil. */
static void fmt_fixed(Fmt *f, double v, int p) {
  if (p < 0)
    p = 0;
  if (p > 18)
    p = 18;
  long long whole = (long long)v;
  double frac0 = v - (double)whole;
  long long s = 1;
  for (int i = 0; i < p; ++i)
    s *= 10;
  long long ri = 0;
  if (p == 0) {
    if (frac0 > 0.5 || (frac0 == 0.5 && (whole & 1)))
      ++whole;
  } else {
    double scaled = frac0 * (double)s;
    long long ftr = (long long)scaled;
    double ffr = scaled - (double)ftr;
    if (ffr > 0.5)
      ri = ftr + 1;
    else if (ffr < 0.5)
      ri = ftr;
    else
      ri = (ftr & 1) ? ftr + 1 : ftr; /* exactly .5: round to even */
    if (ri >= s) {
      ++whole;
      ri = 0;
    }
  }
  char tmp[32];
  int n = 0;
  unsigned long long u = (unsigned long long)whole;
  do {
    tmp[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  while (n)
    fputc_(f, tmp[--n]);
  if (p > 0) {
    fputc_(f, '.');
    char ft[64];
    int fn = 0;
    unsigned long long fu = (unsigned long long)ri;
    do {
      ft[fn++] = (char)('0' + fu % 10);
      fu /= 10;
    } while (fu);
    while (fn < p)
      ft[fn++] = '0';
    while (fn)
      fputc_(f, ft[--fn]);
  }
}

/* Emit 'e<sign><exponent>' with at least two exponent digits. */
static void fmt_exp(Fmt *f, int exp) {
  fputc_(f, 'e');
  int ne = exp < 0 ? -exp : exp;
  fputc_(f, exp < 0 ? '-' : '+');
  char ed[16];
  int n2 = 0;
  do {
    ed[n2++] = (char)('0' + ne % 10);
    ne /= 10;
  } while (ne);
  while (n2 < 2)
    ed[n2++] = '0';
  while (n2)
    fputc_(f, ed[--n2]);
}

/* True when the mantissa text in f rolled over a power of ten: it reads
 * "10" or "10.xxx" (a mantissa in [1,10) that rounded up to 10). */
static int rolled_over(const Fmt *f) {
  size_t i = 0;
  while (i < f->len && f->buf[i] != '.')
    ++i;
  return i == 2 && f->buf[0] == '1' && f->buf[1] == '0';
}

/* %e helper: sign handled by caller; v >= 0. */
static void fmt_sci(Fmt *f, double v, int p) {
  if (p < 0)
    p = 0;
  if (p > 60)
    p = 60;
  int exp = dec_exp(v);
  double m = v / pow10_i(exp);
  char tmp[512];
  Fmt tf;
  tf.buf = tmp;
  tf.cap = sizeof tmp;
  tf.len = 0;
  fmt_fixed(&tf, m, p);
  if (rolled_over(&tf)) {
    ++exp;
    tf.len = 0;
    fmt_fixed(&tf, 1.0, p);
  }
  fputs_(f, tmp, tf.len);
  fmt_exp(f, exp);
}

/* %g trailing-zero strip: drop trailing zeros after the '.', then the '.'. */
static void strip_g(Fmt *f) {
  char *s = f->buf;
  char *dot = NULL;
  for (size_t i = 0; i < f->len; ++i)
    if (s[i] == '.') {
      dot = &s[i];
      break;
    }
  if (!dot)
    return;
  while (f->len > 0 && s[f->len - 1] == '0')
    --f->len;
  if (f->len > 0 && s[f->len - 1] == '.')
    --f->len;
}

/* %g helper: sign handled by caller; v >= 0; P significant digits. */
static void fmt_gen(Fmt *f, double v, int P) {
  if (P < 0)
    P = 0;
  if (P > 60)
    P = 60;
  char tmp[512];
  Fmt tf;
  tf.buf = tmp;
  tf.cap = sizeof tmp;
  tf.len = 0;
  int x = dec_exp(v);
  if (x < -4 || x >= P) {
    int exp = x;
    double m = v / pow10_i(exp);
    fmt_fixed(&tf, m, P - 1);
    if (rolled_over(&tf)) {
      ++exp;
      tf.len = 0;
      fmt_fixed(&tf, 1.0, P - 1);
    }
    strip_g(&tf);
    fputs_(f, tmp, tf.len);
    fmt_exp(f, exp);
  } else {
    int p = P - 1 - x;
    fmt_fixed(&tf, v, p);
    /* If rounding carries the whole part to the next power of ten (e.g.
     * 999.9 -> 1000) and that new exponent E+1 >= P, %g renormalises to
     * scientific notation. Only relevant for values >= 1. */
    size_t wd = 0;
    while (wd < tf.len && tmp[wd] != '.')
      ++wd;
    if (x >= 0 && (int)wd == x + 2 && x + 1 >= P) {
      tf.len = 0;
      fmt_fixed(&tf, 1.0, P - 1);
      strip_g(&tf);
      fputs_(f, tmp, tf.len);
      fmt_exp(f, x + 1);
    } else {
      strip_g(&tf);
      fputs_(f, tmp, tf.len);
    }
  }
}

/* Out of line: these three carry the infinity guard, and inlining them into
 * the shared pli_vsnprintf grows the function that the integer output path
 * also runs through (measured +4.9% on benchmarks/io_throughput.pli). */
#if defined(_MSC_VER)
#define PLI_NOINLINE __declspec(noinline)
#else
#define PLI_NOINLINE __attribute__((noinline))
#endif

PLI_NOINLINE static void fmt_fixed_signed(Fmt *f, double v, int p, int plus) {
  if (v != v) {
    fputs_(f, "nan", 3);
    return;
  }
  /* An infinity has no decimal expansion: printing one would spin in the
   * exponent loop (inf / 10 is still inf). */
  if (is_inf(v)) {
    fputs_(f, v < 0 ? "-inf" : "inf", v < 0 ? 4 : 3);
    return;
  }
  if (is_neg(v)) {
    fputc_(f, '-');
    v = -v;
  } else if (plus) {
    fputc_(f, '+');
  }
  fmt_fixed(f, v, p);
}

PLI_NOINLINE static void fmt_sci_signed(Fmt *f, double v, int p, int plus) {
  if (v != v) {
    fputs_(f, "nan", 3);
    return;
  }
  /* An infinity has no decimal expansion: printing one would spin in the
   * exponent loop (inf / 10 is still inf). */
  if (is_inf(v)) {
    fputs_(f, v < 0 ? "-inf" : "inf", v < 0 ? 4 : 3);
    return;
  }
  if (is_neg(v)) {
    fputc_(f, '-');
    v = -v;
  } else if (plus) {
    fputc_(f, '+');
  }
  fmt_sci(f, v, p);
}

PLI_NOINLINE static void fmt_gen_signed(Fmt *f, double v, int P, int plus) {
  if (v != v) {
    fputs_(f, "nan", 3);
    return;
  }
  /* An infinity has no decimal expansion: printing one would spin in the
   * exponent loop (inf / 10 is still inf). */
  if (is_inf(v)) {
    fputs_(f, v < 0 ? "-inf" : "inf", v < 0 ? 4 : 3);
    return;
  }
  if (is_neg(v)) {
    fputc_(f, '-');
    v = -v;
  } else if (plus) {
    fputc_(f, '+');
  }
  fmt_gen(f, v, P);
}

int pli_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
  Fmt f;
  f.buf = buf;
  f.cap = cap;
  f.len = 0;
  const char *p = fmt;
  while (*p) {
    if (*p != '%') {
      fputc_(&f, *p++);
      continue;
    }
    ++p;
    if (*p == '%') {
      fputc_(&f, '%');
      ++p;
      continue;
    }
    int zero = 0, plus = 0;
    for (;;) {
      if (*p == '0') {
        zero = 1;
        ++p;
      } else if (*p == '+') {
        plus = 1;
        ++p;
      } else {
        break;
      }
    }
    int width = 0;
    if (*p == '*') {
      width = va_arg(ap, int);
      ++p;
    } else {
      while (*p >= '0' && *p <= '9')
        width = width * 10 + (*p++ - '0');
    }
    int prec = -1;
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        prec = va_arg(ap, int);
        ++p;
      } else {
        prec = 0;
        while (*p >= '0' && *p <= '9')
          prec = prec * 10 + (*p++ - '0');
      }
    }
    char conv = *p++;
    int llong = 0;
    if (conv == 'l' && *p == 'l') {
      llong = 1;
      ++p;
      conv = *p++;
    }
    switch (conv) {
    case 'd':
    case 'i': {
      long long iv = llong ? va_arg(ap, long long) : va_arg(ap, int);
      fmt_int(&f, iv, width, zero);
      break;
    }
    case 'f': {
      double dv = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      fmt_fixed_signed(&f, dv, prec, plus);
      break;
    }
    case 'e': {
      double dv = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      fmt_sci_signed(&f, dv, prec, plus);
      break;
    }
    case 'g': {
      double dv = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      fmt_gen_signed(&f, dv, prec, plus);
      break;
    }
    case 's': {
      const char *s2 = va_arg(ap, const char *);
      fputs_(&f, s2, pli_strlen(s2));
      break;
    }
    case 'c':
      fputc_(&f, (char)va_arg(ap, int));
      break;
    default:
      fputc_(&f, conv);
      break;
    }
  }
  if (cap > 0) {
    if (f.len < cap)
      buf[f.len] = '\0';
    else
      buf[cap - 1] = '\0';
  }
  return (int)f.len;
}

int pli_snprintf(char *buf, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = pli_vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return n;
}

/* Assignment to CHARACTER(n) NONVARYING: truncate or pad with blanks. */

