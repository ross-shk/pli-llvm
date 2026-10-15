/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli: a PL/I compiler targeting LLVM */
/* rt_cond.c — PL/I runtime library (pli_runtime): conditions: signal, ON stack/ONCODE, traps. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* stderr printing routes through pli_fprintf (ADR-221); stdio arrives via
 * pli_rt_port.h. */

void pli_signal_error(const char *msg) {
  pli_rt_fini();
  pli_fprintf(stderr, "ERROR condition raised: %s\n", msg ? msg : "(unspecified)");
  pli_exit(8);
}

/* Centralised abort path (Phase 6): flush runtime output, print msg, exit 8. */
void pli_abort(const char *msg) {
  pli_rt_fini();
  pli_fprintf(stderr, "%s\n", msg ? msg : "(unspecified)");
  pli_exit(8);
}

/* ERROR handler stack (rules (91)-(94)): ON pushes a handler entry, REVERT
 * pops, SIGNAL dispatches to the top. Id 0 (and an empty stack) means the
 * system action. Programmer-named conditions (rules (94),(99)) share the
 * stack as tagged entries (key 0 is ERROR). Each entry carries the compiled
 * handler address plus its capture context (rule 91): the establishing-frame
 * addresses of automatics the unit touches, or NULL when the unit touches
 * none. A SIGNAL runs the topmost handler established for its own condition
 * — wherever it was pushed, including another object file — then resumes.
 * Thread-local (QR2.8): each task owns its handler stack and ONCODE. */
#define PLI_ON_MAX 64
static _Thread_local struct {
  long long key; /* 0 = ERROR, else the sema-assigned condition key */
  long long id;  /* handler id; 0 = the system action */
  void *fn;      /* compiled handler address; NULL = system action */
  void *ctx;     /* capture context; NULL when the unit touches no automatics */
} pli_err_stack[PLI_ON_MAX];
static _Thread_local int pli_err_sp = 0;
static _Thread_local int pli_oncode_val = 0;
static _Thread_local int pli_on_source_key = 0;
void rt_pli_on_push(long long key, long long id, void *fn, void *ctx) {
  if (pli_err_sp < PLI_ON_MAX) {
    pli_err_stack[pli_err_sp].key = key;
    pli_err_stack[pli_err_sp].id = id;
    pli_err_stack[pli_err_sp].fn = fn;
    pli_err_stack[pli_err_sp].ctx = ctx;
    ++pli_err_sp;
  } else {
    pli_abort("ON ERROR stack overflow");
  }
}
/* Topmost id for a key, or 0 when none (or SYSTEM) is established for it. */
long long rt_pli_on_top(long long key) {
  for (int i = pli_err_sp - 1; i >= 0; --i)
    if (pli_err_stack[i].key == key)
      return pli_err_stack[i].id;
  return 0;
}
/* Topmost handler address for a key, or NULL when none (or SYSTEM). */
void *rt_pli_on_top_fn(long long key) {
  for (int i = pli_err_sp - 1; i >= 0; --i)
    if (pli_err_stack[i].key == key)
      return pli_err_stack[i].fn;
  return NULL;
}
/* Capture context of the topmost handler for a key (NULL when none). */
void *rt_pli_on_top_ctx(long long key) {
  for (int i = pli_err_sp - 1; i >= 0; --i)
    if (pli_err_stack[i].key == key)
      return pli_err_stack[i].ctx;
  return NULL;
}
/* Drop the topmost entry for a key; a no-op when none is established. */
void rt_pli_on_pop(long long key) {
  for (int i = pli_err_sp - 1; i >= 0; --i)
    if (pli_err_stack[i].key == key) {
      for (int j = i; j < pli_err_sp - 1; ++j)
        pli_err_stack[j] = pli_err_stack[j + 1];
      --pli_err_sp;
      return;
    }
}
void pli_on_push_error(long long id, char *fn, char *ctx) {
  pli_on_push(0, id, (void*)fn, (void*)ctx);
}
void pli_on_pop_error(void) {
  pli_on_pop(0);
}
long long pli_on_top_error(void) {
  return pli_on_top(0);
}
void pli_on_push_cond(long long key, long long id, char *fn, char *ctx) {
  pli_on_push(key, id, (void*)fn, (void*)ctx);
}
void pli_on_pop_cond(long long key) {
  pli_on_pop(key);
}
long long pli_on_top_cond(long long key) {
  return pli_on_top(key);
}
char *pli_on_top_fn(long long key) {
  return (char*)rt_pli_on_top_fn(key);
}
char *pli_on_top_ctx(long long key) {
  return (char*)rt_pli_on_top_ctx(key);
}
long long pli_on_depth_error(void) {
  return pli_err_sp;
}
void pli_on_reset_error(long long d) {
  if (d >= 0 && d <= pli_err_sp)
    pli_err_sp = (int)d;
}
int pli_oncode(void) {
  return pli_oncode_val;
}
void pli_set_oncode(int c) {
  pli_oncode_val = c;
}

/* ONKEY() (rule (123), Appendix 1): returns BIT(1) = 1 if a SIGNAL is
   currently being handled (i.e. we're inside an ON handler unit). */
int pli_onkey(void) {
  return pli_err_sp > 0 ? 1 : 0;
}

/* ONSOURCE() (rule (123), Appendix 1): returns the key of the current
   SIGNAL's source condition (0 = SYSTEM/none). */
int pli_on_source(void) {
  if (pli_err_sp > 0)
    return (int)pli_err_stack[pli_err_sp - 1].key;
  return pli_on_source_key;
}

/* ONSOURCE = key (TR 123, ADR-210): set the source condition key of the
   currently-handled SIGNAL. If inside an ON handler, sets the topmost stack
   entry; otherwise stores a pending key for later ONSOURCE() reads. */
void pli_set_on_source(int key) {
  if (pli_err_sp > 0)
    pli_err_stack[pli_err_sp - 1].key = (long long)key;
  else
    pli_on_source_key = key;
}

/* ONFILE() (rule (123), Appendix 1): the file of the current I/O condition.
   No file-condition tracking exists while ON ENDFILE/KEY stays diagnosed, so
   every reachable call is out-of-context -> blank (IBM's null string). */
void pli_onfile(char *buf, long long cap) {
  for (long long i = 0; i < cap; ++i)
    buf[i] = ' ';
}

/* UNSPEC(var) = bits (rule (123), ADR-196): copy raw storage. Copies min(srcLen,
   dstSize) bytes; remaining dst bytes are zero-filled when src is shorter. */
void pli_unspec_assign(char *dst, long long dstSize, char *src, long long srcLen) {
  long long n = srcLen < dstSize ? srcLen : dstSize;
  pli_memcpy(dst, src, (size_t)n);
  if (n < dstSize) {
    char *tail = dst + n;
    for (long long i = 0; i < dstSize - n; ++i)
      tail[i] = 0;
  }
}

/* SUBSCRIPTRANGE abort (no handler): a runtime subscript is out of bounds.
 * Reached when no SUBSCRIPTRANGE handler is established; otherwise IRGen
 * routes the trap to the handler and resumes with the index clamped. */
void pli_subscript_oob(void) {
  pli_abort("SUBSCRIPTRANGE: array subscript out of bounds");
}

/* ZERODIVIDE abort (no handler): division or modulo by zero. Reached when no
 * ZERODIVIDE handler is established; otherwise IRGen routes the trap to the
 * handler and resumes with 0. */
void pli_zerodivide(void) {
  pli_abort("ZERODIVIDE: division by zero");
}

/* FIXED overflow (QR1.2: binary arithmetic, decimal precision, float to
 * fixed conversions): abort path of the SIZE dispatch (QR1.4, ADR-097).
 * Reached when no SIZE handler is established; otherwise IRGen routes the
 * trap to the SIZE handler and resumes. */
void pli_fixed_overflow(void) {
  pli_signal_error("FIXED overflow");
}

/* CONVERSION abort (rule 94, ADR-170): character-to-fixed conversion of text
 * with no valid digits. Reached when no CONVERSION handler is established;
 * otherwise IRGen routes the trap to the CONVERSION handler and resumes with 0. */
void pli_conversion(void) {
  pli_signal_error("CONVERSION: invalid numeric conversion");
}

/* ALLOCATE (rule 87): heap-allocate n bytes for a based structure. A null
 * return would be an ALLOCATION condition (M4); the interim raises a hard
 * error. */

