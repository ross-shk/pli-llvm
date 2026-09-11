/* pli_rt.c — PL/I runtime library (libpli), M0 subset. */
#include "pli_rt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SYSPRINT state. A full implementation tracks page/line/column against
 * LINESIZE and PAGESIZE and raises ENDPAGE; M0 tracks the column only. */
static int col = 0;
static int items_on_line = 0;

/* STRING (rule 105) sink/source: when out_buf is non-null, list-directed output
 * is written into it instead of stdout; when in_buf is non-null, list-directed
 * input is read from it instead of stdin. */
static char *out_buf = NULL;
static size_t out_cap = 0, out_len = 0;
static const char *in_buf = NULL;
static size_t in_len = 0, in_pos = 0;

/* Named files (rules 100-103, 105): a fixed table indexed by the compile-time
 * slot assigned to each FILE variable. out_f/in_f are the streams selected by
 * the FILE ( f ) option of a PUT/GET; when set they override stdout/stdin. */
#define PLI_MAX_FILES 16
static FILE *pli_files[PLI_MAX_FILES] = {0};
static FILE *out_f = NULL;
static FILE *in_f = NULL;

static void put_raw(const char *p, size_t n) {
  if (out_f) {
    fwrite(p, 1, n, out_f);
    return;
  }
  if (out_buf) {
    size_t take = n < (out_cap - out_len) ? n : (out_cap - out_len);
    if (take > 0)
      memmove(out_buf + out_len, p, take);
    out_len += take;
    return;
  }
  fwrite(p, 1, n, stdout);
  col += (int)n;
}

/* Read one input character from the active FILE stream, STRING source, or
 * stdin. */
static int next_char(void) {
  if (in_f)
    return getc(in_f);
  if (in_buf)
    return in_pos < in_len ? (unsigned char)in_buf[in_pos++] : EOF;
  return getchar();
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
  if (v < 0) { *p++ = '-'; v = -v; }
  long long factor = 1LL << scale;  /* 2^scale */
  int n = snprintf(p, sizeof buf - (size_t)(p - buf), "%lld", v / factor);
  p += n;
  long long rem = v % factor;
  if (rem != 0) {
    *p++ = '.';
    char digit[64];
    int cnt = 0;
    while (rem != 0 && cnt < 60) {
      rem *= 10;
      digit[cnt++] = (char)(rem >> scale);
      rem &= (factor - 1);
    }
    for (int i = 0; i < cnt; ++i) *p++ = '0' + digit[i];
  }
  separate();
  put_raw(buf, (size_t)(p - buf));
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

/* SUBSTR(s, i, n): copy up to n characters of s starting at the 1-based
 * position i, blank-filling the tail. Positions past the end of s clip to
 * blanks (a real compiler would raise SUBSCRIPTRANGE, M3). */
void pli_substr(char *dst, long long dstcap, const char *src, long long srclen,
                long long start, long long len) {
  long long n = len < dstcap ? len : dstcap;
  long long avail = srclen - (start - 1);
  if (avail < 0) avail = 0;
  long long take = avail < n ? avail : n;
  if (take > 0 && start >= 1) memmove(dst, src + (start - 1), (size_t)take);
  if (n > take) memset(dst + take, ' ', (size_t)(n - take));
}

/* SUBSTR(s, i, n) = v: overwrite n characters of dst starting at the 1-based
 * position i with the first n characters of src, blank-filling the tail when
 * src is shorter than n. Positions past the end of dst clip (SUBSCRIPTRANGE
 * is M3). */
void pli_substr_assign(char *dst, long long dstcap, long long start, long long len,
                       const char *src, long long srclen) {
  if (start < 1) return;
  long long n = len;
  long long space = dstcap - (start - 1);
  if (space <= 0) return;
  if (n > space) n = space;
  long long take = srclen < n ? srclen : n;
  if (take > 0) memmove(dst + (start - 1), src, (size_t)take);
  if (n > take) memset(dst + (start - 1) + take, ' ', (size_t)(n - take));
}

long long pli_mod_ll(long long a, long long b) {
  if (b == 0) return 0;
  long long r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) r += b;  /* result takes the sign of b */
  return r;
}

/* MOD (float): a - b*floor(a/b); the result takes the sign of b. */
double pli_mod_dd(double a, double b) {
  double r = fmod(a, b);
  if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
  return r;
}

/* ROUND(x, n): round x to n fractional decimal digits (n may be negative). */
double pli_round(double x, long long n) {
  double factor = 1.0;
  for (long long i = 0; i < n; ++i) factor *= 10.0;
  for (long long i = 0; i > n; --i) factor /= 10.0;
  return round(x * factor) / factor;
}

/* Scalar math built-ins (QR2.7, <math.h> analogues): thin wrappers so the
 * emitted IR ABI is the pli_* one. LOG is the natural logarithm. */
double pli_floor(double x) { return floor(x); }
double pli_ceil(double x) { return ceil(x); }
double pli_sqrt(double x) { return sqrt(x); }
double pli_exp(double x) { return exp(x); }
double pli_log(double x) { return log(x); }
double pli_sin(double x) { return sin(x); }
double pli_cos(double x) { return cos(x); }
double pli_tan(double x) { return tan(x); }
double pli_log2(double x) { return log2(x); }
double pli_log10(double x) { return log10(x); }
double pli_atan(double x) { return atan(x); }
double pli_sinh(double x) { return sinh(x); }
double pli_cosh(double x) { return cosh(x); }
double pli_tanh(double x) { return tanh(x); }
double pli_atanh(double x) { return atanh(x); }
double pli_erf(double x) { return erf(x); }
double pli_erfc(double x) { return erfc(x); }

/* Degree trig variants (QR2.7, Appendix 1): argument/result in degrees. */
#define PLI_DEG_TO_RAD (3.14159265358979323846 / 180.0)
#define PLI_RAD_TO_DEG (180.0 / 3.14159265358979323846)
double pli_sind(double x) { return sin(x * PLI_DEG_TO_RAD); }
double pli_cosd(double x) { return cos(x * PLI_DEG_TO_RAD); }
double pli_tand(double x) { return tan(x * PLI_DEG_TO_RAD); }
double pli_atand(double x) { return atan(x) * PLI_RAD_TO_DEG; }

/* REPEAT(s, n): fill dst with n copies of s, blank-padding to dstcap. */
void pli_repeat(char *dst, long long dstcap, const char *src, long long srclen,
                long long n) {
  long long out = 0;
  for (long long k = 0; k < n; ++k)
    for (long long i = 0; i < srclen && out < dstcap; ++i) dst[out++] = src[i];
  while (out < dstcap) dst[out++] = ' ';
}

/* VERIFY(s, t): 1-based position of the first character of s not in t, or 0
 * if every character of s appears in t (Y33-6003 VERIFY). */
long long pli_verify(const char *s, long long slen, const char *t, long long tlen) {
  for (long long i = 0; i < slen; ++i) {
    long long j = 0;
    while (j < tlen && s[i] != t[j]) ++j;
    if (j == tlen) return i + 1;
  }
  return 0;
}

/* TRANSLATE(s, out, in): copy s, mapping each character that occurs in `in`
 * to the corresponding character of `out`; unmatched characters pass through. */
void pli_translate(char *dst, long long dstcap, const char *s, long long slen,
                   const char *out, long long outlen, const char *in, long long inlen) {
  for (long long i = 0; i < slen && i < dstcap; ++i) {
    char c = s[i];
    long long k = 0;
    while (k < inlen && c != in[k]) ++k;
    if (k < inlen && k < outlen) c = out[k];
    dst[i] = c;
  }
}

/* HIGH(n): n copies of the highest collating character (0xFF). */
void pli_high(char *dst, long long n) { memset(dst, 0xFF, (size_t)n); }

/* LOW(n): n copies of the lowest collating character (0x00). */
void pli_low(char *dst, long long n) { memset(dst, 0x00, (size_t)n); }

/* DATE(): write the current local date as 'YYYYMMDD'. */
void pli_date(char *buf, long long cap) {
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char tmp[16];
  int n = snprintf(tmp, sizeof tmp, "%04d%02d%02d",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  long long i = 0;
  for (; i < cap && i < n; ++i) buf[i] = tmp[i];
  for (; i < cap; ++i) buf[i] = ' ';
}

/* TIME(): write the current local time as 'HHMMSS'. */
void pli_time(char *buf, long long cap) {
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char tmp[16];
  int n = snprintf(tmp, sizeof tmp, "%02d%02d%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  long long i = 0;
  for (; i < cap && i < n; ++i) buf[i] = tmp[i];
  for (; i < cap; ++i) buf[i] = ' ';
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

/* INDEX(s1, s2): 1-based position of the first occurrence of s2 within s1, or
 * 0 if not present. An empty s2 matches at position 1 (Y33-6003 INDEX). */
long long pli_index(const char *a, long long alen, const char *b, long long blen) {
  if (blen <= 0) return 1;
  if (blen > alen) return 0;
  for (long long i = 0; i + blen <= alen; ++i) {
    long long j = 0;
    while (j < blen && a[i + j] == b[j]) ++j;
    if (j == blen) return i + 1;
  }
  return 0;
}
void pli_signal_error(const char *msg) {
  pli_rt_fini();
  fprintf(stderr, "ERROR condition raised: %s\n", msg ? msg : "(unspecified)");
  exit(8);
}

/* SUBSCRIPTRANGE interim (M2): a runtime subscript is out of bounds. Raised as
 * a hard error until condition handling (M4) provides ON SUBSCRIPTRANGE. */
void pli_subscript_oob(void) {
  pli_rt_fini();
  fprintf(stderr, "SUBSCRIPTRANGE: array subscript out of bounds\n");
  exit(8);
}

/* ALLOCATE (rule 87): heap-allocate n bytes for a based structure. A null
 * return would be an ALLOCATION condition (M4); the interim raises a hard
 * error. */
char *pli_alloc(long long n) {
  char *p = (char *)malloc((size_t)(n < 0 ? 0 : n));
  if (!p) {
    pli_rt_fini();
    fprintf(stderr, "ALLOCATION: out of memory\n");
    exit(8);
  }
  return p;
}

/* FREE (rule 90): release the heap block addressed by a based pointer. */
void pli_free(char *p) { free(p); }

/* List-directed input (rule 109): read the next whitespace/comma-delimited
 * token from SYSIN (stdin) into buf (nul-terminated). Returns 0 at end of
 * input. */
static int get_token(char *buf, size_t cap) {
  int c;
  do {
    c = next_char();
  } while (c != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
  if (c == EOF)
    return 0;
  size_t n = 0;
  while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
    if (n + 1 < cap)
      buf[n++] = (char)c;
    c = next_char();
  }
  buf[n] = '\0';
  return 1;
}

long long pli_get_list_fixed(void) {
  char tok[64];
  if (!get_token(tok, sizeof tok))
    return 0;
  return strtoll(tok, NULL, 10);
}

double pli_get_list_float(void) {
  char tok[64];
  if (!get_token(tok, sizeof tok))
    return 0.0;
  return strtod(tok, NULL);
}

void pli_get_list_char(char *dst, long long cap) {
  char tok[256];
  get_token(tok, sizeof tok);
  long long i = 0;
  for (; i < cap && tok[i]; ++i)
    dst[i] = tok[i];
  for (; i < cap; ++i)
    dst[i] = ' ';
}

unsigned char pli_get_list_bit(void) {
  char tok[16];
  if (!get_token(tok, sizeof tok))
    return 0;
  return tok[0] == '1' ? 1 : 0;
}

/* STRING (rule 105) PUT: route list-directed output into buf (cap bytes). */
void pli_string_put_open(char *buf, long long cap) {
  out_buf = buf;
  out_cap = (size_t)cap;
  out_len = 0;
  items_on_line = 0;
}

/* End a STRING PUT: blank-pad the unused tail, then return to SYSPRINT. */
void pli_string_put_close(char *buf, long long cap) {
  (void)buf;
  if (out_buf && out_len < (size_t)cap)
    memset(out_buf + out_len, ' ', (size_t)cap - out_len);
  out_buf = NULL;
  out_cap = out_len = 0;
}

/* STRING (rule 105) GET: read list-directed input from buf (len bytes). */
void pli_string_get_open(char *buf, long long len) {
  in_buf = buf;
  in_len = (size_t)len;
  in_pos = 0;
}

/* End a STRING GET: return to SYSIN. */
void pli_string_get_close(void) {
  in_buf = NULL;
  in_len = in_pos = 0;
}

/* OPEN (rules 100-103): open the file in `slot` with the given name (namelen
 * bytes, not nul-terminated). mode 0 = INPUT, 1 = OUTPUT. The FILE variable's
 * slot is a compile-time constant, so pli_files[slot] is stable across the
 * OPEN/GET/PUT/CLOSE statements that name it. */
void pli_file_open(long long slot, const char *name, long long namelen, long long mode) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  char buf[256];
  size_t n = namelen < (long long)(sizeof buf - 1) ? (size_t)namelen : sizeof buf - 1;
  memcpy(buf, name, n);
  buf[n] = '\0';
  if (pli_files[slot])
    fclose(pli_files[slot]);
  pli_files[slot] = fopen(buf, mode ? "w" : "r");
}

/* CLOSE (rule 102): close the file in `slot` and release the slot. */
void pli_file_close(long long slot) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  if (pli_files[slot]) {
    fclose(pli_files[slot]);
    pli_files[slot] = NULL;
  }
}

/* FILE ( f ) output routing (rule 105): put_raw writes to the named file until
 * the matching unselect returns to SYSPRINT. */
void pli_put_select(long long slot) {
  out_f = (slot >= 0 && slot < PLI_MAX_FILES) ? pli_files[slot] : NULL;
  col = 0;
  items_on_line = 0;
}

void pli_put_unselect(void) {
  out_f = NULL;
}

/* FILE ( f ) input routing (rule 105): next_char reads from the named file. */
void pli_get_select(long long slot) {
  in_f = (slot >= 0 && slot < PLI_MAX_FILES) ? pli_files[slot] : NULL;
}

void pli_get_unselect(void) {
  in_f = NULL;
}

/* Edit-directed output (rules (108),(44)-(54)). All output routes through
 * put_raw so STRING/FILE sources and sinks are honoured. */

/* Right-justify a formatted string into a field of width w: pad on the left
 * with blanks; if the content is wider than w, write it in full. */
static void put_field(const char *p, size_t n, long long w) {
  if (w > (long long)n)
    for (long long i = n; i < w; ++i) put_raw(" ", 1);
  put_raw(p, n);
}

/* F(w,d) on a FIXED value stored as value*10^scale: rescale to exactly d
 * fractional digits (rounding half away from zero on a downward rescale), then
 * right-justify the result in width w. */
void pli_put_edit_fixed(long long v, long long scale, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  long long sc = scale - d;
  if (sc > 0) { /* divide by 10^sc, rounding half away from zero */
    long long f = 1, t = sc;
    while (t--) f *= 10;
    int neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
    unsigned long long q = u / (unsigned long long)f, r = u % (unsigned long long)f;
    if (r * 2 >= (unsigned long long)f)
      ++q;
    v = neg ? -(long long)q : (long long)q;
  } else if (sc < 0) {
    long long f = 1, t = -sc;
    while (t--) f *= 10;
    v *= f;
  }
  char buf[96];
  char *p = buf;
  if (d > 0) {
    long long f = 1, t = d;
    while (t--) f *= 10;
    long long ip = v / f, fr = v % f;
    if (fr < 0) fr = -fr;
    int n = snprintf(p, (size_t)(buf + sizeof buf - p), "%lld", ip);
    p += n;
    *p++ = '.';
    char frac[64];
    int fn = snprintf(frac, sizeof frac, "%0*lld", (int)d, fr);
    for (int i = 0; i < fn; ++i) *p++ = frac[i];
  } else {
    int n = snprintf(p, (size_t)(buf + sizeof buf - p), "%lld", v);
    p += n;
  }
  put_field(buf, (size_t)(p - buf), w);
}

/* F(w,d) on a FLOAT value: d fractional digits, right-justified in width w. */
void pli_put_edit_float(double v, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  char buf[128];
  int n = snprintf(buf, sizeof buf, "%.*f", (int)d, v);
  put_field(buf, (size_t)n, w);
}

/* E(w,d) on a FLOAT value: scientific notation with one leading digit, d
 * fractional digits, and a two-digit signed exponent (e.g. 1.25E+04), right-
 * justified in width w. `%.*e` yields `d.dddde±e`; the exponent is normalised
 * to at least two digits (0-padded) per the E-format convention. */
void pli_put_edit_float_e(double v, long long w, long long d) {
  if (d < 0) d = 0;
  if (d > 60) d = 60;
  char tmp[128];
  snprintf(tmp, sizeof tmp, "%.*e", (int)d, v);
  char buf[128];
  char *p = buf;
  const char *q = tmp;
  if (*q == '-') { *p++ = *q++; }        /* sign */
  const char *dot = strchr(q, 'e');
  size_t mant = (size_t)(dot ? dot - q : strlen(q));
  memcpy(p, q, mant); p += mant;          /* d.dddd mantissa */
  if (!dot) { *p = '\0'; put_field(buf, (size_t)(p - buf), w); return; }
  const char *e = dot + 1;                /* e.g. "+04" or "-12" */
  int ex = atoi(e);
  int neg = ex < 0;
  if (neg) ex = -ex;
  *p++ = 'E';
  *p++ = neg ? '-' : '+';
  if (ex < 10) *p++ = '0';
  p += snprintf(p, (size_t)(buf + sizeof buf - p), "%d", ex);
  put_field(buf, (size_t)(p - buf), w);
}

/* A(w): a character value right-justified in width w, truncated on the right
 * when longer than the field. */
void pli_put_edit_char(const char *p, long long len, long long w) {
  long long take = len < w ? len : w;
  if (w > take)
    for (long long i = take; i < w; ++i) put_raw(" ", 1);
  if (take > 0)
    put_raw(p, (size_t)take);
}

/* X(w): w blank characters. */
void pli_put_edit_x(long long w) {
  for (long long i = 0; i < w; ++i) put_raw(" ", 1);
}

/* SKIP(n): advance to the start of a line and then n-1 further lines. */
void pli_put_edit_skip(long long n) {
  if (n < 1) n = 1;
  for (long long i = 0; i < n; ++i) put_raw("\n", 1);
}

/* PAGE: a form feed. */
void pli_put_edit_page(void) { put_raw("\f", 1); }

/* LINE(n): advance so the next item is on line n (1-based). A line counter is
 * not tracked, so this emits (n-1) newlines as an approximation. */
void pli_put_edit_line(long long n) {
  if (n < 1) n = 1;
  for (long long i = 1; i < n; ++i) put_raw("\n", 1);
}

/* Edit-directed input (rules (108),(44)-(54)). */

/* F(w,d): read a fixed-width field of w characters and parse it as a number.
 * The caller converts the double to the target's type. */
double pli_get_edit_num(long long w) {
  char field[128];
  long long i = 0;
  for (; i < w && i < (long long)(sizeof field - 1); ++i) {
    int c = next_char();
    if (c == EOF)
      break;
    field[i] = (char)c;
  }
  field[i] = '\0';
  return strtod(field, NULL);
}

/* A(w): read w characters left-justified into dst, blank-padded to cap; a field
 * wider than the target is truncated (the excess input is still consumed). */
void pli_get_edit_char(char *dst, long long cap, long long w) {
  long long i = 0;
  for (; i < w; ++i) {
    int c = next_char();
    if (c == EOF)
      break;
    if (i < cap)
      dst[i] = (char)c;
  }
  for (; i < cap; ++i)
    dst[i] = ' ';
}

/* X(w): skip w input characters. */
void pli_get_edit_x(long long w) {
  for (long long i = 0; i < w; ++i) next_char();
}

/* SKIP(n): consume input up to and including the nth newline. */
void pli_get_edit_skip(long long n) {
  if (n < 1) n = 1;
  int nl = 0;
  while (nl < n) {
    int c = next_char();
    if (c == EOF)
      break;
    if (c == '\n')
      ++nl;
  }
}

