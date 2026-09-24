/* rt_storage.c — PL/I runtime library (libpli): heap: ALLOCATE/FREE. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdlib.h>

char *pli_alloc(long long n) {
  char *p = (char *)malloc((size_t)(n < 0 ? 0 : n));
  if (!p)
    pli_abort("ALLOCATION: out of memory");
  return p;
}

/* FREE (rule 90): release the heap block addressed by a based pointer. */
void pli_free(char *p) { free(p); }

/* CONTROLLED generation stack (rules 87-90, ADR-140): LIFO generations per
 * variable, indexed by the compile-time slot sema assigns (fileSlot
 * precedent). References always address generation depth-1; generations
 * outlive blocks and only explicit FREE pops. */
typedef struct {
  char **gens;
  long long depth, cap;
} CtlVar;
static CtlVar *ctlVars = NULL;
static long long ctlCount = 0;

static CtlVar *ctlEnsure(long long slot) {
  if (slot < 0)
    pli_signal_error("CONTROLLED slot out of range");
  if (slot >= ctlCount) {
    long long ncap = slot + 16;
    CtlVar *nv = (CtlVar *)realloc(ctlVars, (size_t)ncap * sizeof(CtlVar));
    if (!nv)
      pli_signal_error("CONTROLLED table out of memory");
    for (long long i = ctlCount; i < ncap; ++i) {
      nv[i].gens = NULL;
      nv[i].depth = 0;
      nv[i].cap = 0;
    }
    ctlVars = nv;
    ctlCount = ncap;
  }
  return &ctlVars[slot];
}

/* ALLOCATE a CONTROLLED generation (rule 87): push a fresh block sized by
 * the caller from the compile-time descriptor. */
void pli_ctl_alloc(long long slot, long long n) {
  CtlVar *v = ctlEnsure(slot);
  if (v->depth >= v->cap) {
    long long ncap = v->cap ? v->cap * 2 : 4;
    char **ng = (char **)realloc(v->gens, (size_t)ncap * sizeof(char *));
    if (!ng)
      pli_signal_error("CONTROLLED generation stack out of memory");
    v->gens = ng;
    v->cap = ncap;
  }
  v->gens[v->depth++] = pli_alloc(n);
}

/* FREE a CONTROLLED generation (rule 90): pop; empty stack is diagnosed,
 * never silently ignored (invariant 2). */
void pli_ctl_free(long long slot) {
  if (slot < 0 || slot >= ctlCount || ctlVars[slot].depth <= 0)
    pli_signal_error("FREE of empty CONTROLLED stack");
  CtlVar *v = &ctlVars[slot];
  free(v->gens[--v->depth]);
}

/* Latest CONTROLLED generation address, or NULL when none is allocated. */
char *pli_ctl_addr(long long slot) {
  if (slot < 0 || slot >= ctlCount || ctlVars[slot].depth <= 0)
    return NULL;
  return ctlVars[slot].gens[ctlVars[slot].depth - 1];
}

