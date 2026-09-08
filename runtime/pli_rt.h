/* pli_rt.h — PL/I runtime library (libpli), M0 subset.
 *
 * The compiler emits calls to these entry points. Names are the ABI: see
 * docs/ARCHITECTURE.md "Runtime interface".
 */
#ifndef PLI_RT_H
#define PLI_RT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Program prologue/epilogue. */
void pli_rt_init(void);
void pli_rt_fini(void);
void pli_stop(void); /* STOP statement, rule (85) */

/* SYSPRINT stream output, list-directed (rules 104-109). */
void pli_put_skip(long long n);
void pli_put_page(void);
void pli_put_list_char(const char *p, long long len);
void pli_put_list_fixed(long long v);
void pli_put_list_float(double v);
void pli_put_list_bit(unsigned char b);

/* String assignment and comparison (blank padding semantics). */
void pli_assign_char(char *dst, long long dstlen, const char *src, long long srclen);
long long pli_assign_varying(char *dstdata, long long cap, const char *src, long long srclen);
void pli_concat(char *dst, const char *a, long long alen, const char *b, long long blen);
void pli_substr(char *dst, long long dstcap, const char *src, long long srclen,
                long long start, long long len); /* SUBSTR built-in */
long long pli_index(const char *a, long long alen, const char *b, long long blen); /* INDEX */
long long pli_mod_ll(long long a, long long b);  /* MOD (integer) */
double pli_mod_dd(double a, double b);           /* MOD (float) */
double pli_round(double x, long long n);         /* ROUND */
void pli_repeat(char *dst, long long dstcap, const char *src, long long srclen,
                long long n);                    /* REPEAT */
long long pli_verify(const char *s, long long slen, const char *t, long long tlen); /* VERIFY */
void pli_translate(char *dst, long long dstcap, const char *s, long long slen,
                   const char *out, long long outlen, const char *in, long long inlen); /* TRANSLATE */
void pli_high(char *dst, long long n);  /* HIGH(n): n copies of the top char */
void pli_low(char *dst, long long n);   /* LOW(n): n copies of the bottom char */
int pli_cmp_char(const char *a, long long alen, const char *b, long long blen);

/* Condition signalling (M5 will expand this into the ON-unit machinery). */
void pli_signal_error(const char *msg);

#ifdef __cplusplus
}
#endif
#endif /* PLI_RT_H */
