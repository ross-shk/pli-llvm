/* pli_rt.c — PL/I runtime library (libpli), M0 subset. */
#include "pli_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SYSPRINT state. A full implementation tracks page/line/column against
 * LINESIZE and PAGESIZE and raises ENDPAGE; M0 tracks the column only. */
static int col = 0;
static int items_on_line = 0;

static void put_raw(const char *p, size_t n) {
  fwrite(p, 1, n, stdout);
  col += (int)n;
}

void pli_rt_init(void) {
  col = 0;
  items_on_line = 0;
}

void pli_rt_fini(void) {
  if (col > 0) {
    fputc('\n', stdout);
    col = 0;
  }
  fflush(stdout);
}

void pli_stop(void) {
  pli_rt_fini();
  exit(0);
}

/* SKIP(n): advance to the start of a line, then n-1 further lines.
 * M0 deviation: on a fresh line SKIP does not emit a blank line, so the
 * idiomatic `PUT SKIP LIST(...)` prints on the current line. */
void pli_put_skip(long long n) {
  if (n < 1) n = 1;
  if (col > 0) {
    fputc('\n', stdout);
    col = 0;
  }
  for (long long i = 1; i < n; ++i) fputc('\n', stdout);
  items_on_line = 0;
}

void pli_put_page(void) {
  if (col > 0) fputc('\n', stdout);
  fputc('\f', stdout);
  col = 0;
  items_on_line = 0;
}

/* List-directed output separates successive items by a blank; character
 * values are written without enclosing quotation marks. */
static void separate(void) {
  if (items_on_line > 0) put_raw(" ", 1);
  items_on_line++;
}

void pli_put_list_char(const char *p, long long len) {
  separate();
  if (len > 0) put_raw(p, (size_t)len);
}

void pli_put_list_fixed(long long v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%lld", v);
  separate();
  put_raw(buf, (size_t)n);
}

void pli_put_list_float(double v) {
  char buf[64];
  /* PL/I would use E-format for FLOAT; %.6g keeps the wireframe readable. */
  int n = snprintf(buf, sizeof buf, "%.6g", v);
  separate();
  put_raw(buf, (size_t)n);
}

void pli_put_list_bit(unsigned char b) {
  separate();
  put_raw(b ? "1" : "0", 1);
}

/* Assignment to CHARACTER(n) NONVARYING: truncate or pad with blanks. */
void pli_assign_char(char *dst, long long dstlen, const char *src, long long srclen) {
  long long n = srclen < dstlen ? srclen : dstlen;
  if (n > 0) memmove(dst, src, (size_t)n);
  if (dstlen > n) memset(dst + n, ' ', (size_t)(dstlen - n));
}

/* Assignment to CHARACTER(n) VARYING: truncate to the maximum length; the
 * caller stores the returned current length. */
long long pli_assign_varying(char *dstdata, long long cap, const char *src, long long srclen) {
  long long n = srclen < cap ? srclen : cap;
  if (n > 0) memmove(dstdata, src, (size_t)n);
  return n;
}

void pli_concat(char *dst, const char *a, long long alen, const char *b, long long blen) {
  if (alen > 0) memmove(dst, a, (size_t)alen);
  if (blen > 0) memmove(dst + alen, b, (size_t)blen);
}

/* Comparison of character data: the shorter operand is notionally extended
 * with blanks on the right. */
int pli_cmp_char(const char *a, long long alen, const char *b, long long blen) {
  long long n = alen > blen ? alen : blen;
  for (long long i = 0; i < n; ++i) {
    unsigned char ca = i < alen ? (unsigned char)a[i] : (unsigned char)' ';
    unsigned char cb = i < blen ? (unsigned char)b[i] : (unsigned char)' ';
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  return 0;
}

void pli_signal_error(const char *msg) {
  pli_rt_fini();
  fprintf(stderr, "ERROR condition raised: %s\n", msg ? msg : "(unspecified)");
  exit(8);
}
