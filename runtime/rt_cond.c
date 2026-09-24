/* rt_cond.c — PL/I runtime library (libpli): conditions: signal, ON stack/ONCODE, traps. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>

void pli_signal_error(const char *msg) {
  pli_rt_fini();
  fprintf(stderr, "ERROR condition raised: %s\n", msg ? msg : "(unspecified)");
  pli_exit(8);
}

/* Centralised abort path (Phase 6): flush runtime output, print msg, exit 8. */
void pli_abort(const char *msg) {
  pli_rt_fini();
  fprintf(stderr, "%s\n", msg ? msg : "(unspecified)");
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

/* ALLOCATE (rule 87): heap-allocate n bytes for a based structure. A null
 * return would be an ALLOCATION condition (M4); the interim raises a hard
 * error. */

