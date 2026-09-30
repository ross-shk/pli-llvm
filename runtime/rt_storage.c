/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* rt_storage.c — PL/I runtime library (libpli): heap: ALLOCATE/FREE. */
#include "pli_rt.h"
#include <stdlib.h>

char *pli_alloc(long long n) {
  char *p = (char *)malloc((size_t)(n < 0 ? 0 : n));
  if (!p)
    pli_abort("ALLOCATION: out of memory");
  return p;
}

void pli_free(char *p) { free(p); }

typedef struct {
  long long key; /* the variable's deterministic name hash; -1 = empty entry */
  char **gens;
  long long *lens;
  /* Per-generation N-D extents (rules (13),(89)): rank[k] is the axis count of
   * generation k and dimss[k] its extents (NULL when rank is 0, i.e. a 1-D
   * generation sized via pli_ctl_len or a non-array generation). Lower bounds
   * are static (the DECLARE lb, else 1), so only extents vary per generation. */
  long long *ranks;
  long long **dimss;
  long long depth, cap;
} CtlVar;
static CtlVar *ctlVars = NULL;
static long long ctlCount = 0;

/* Find (or, when create is set, append) the generation stack for a variable.
 * Keys are the sema-assigned FNV-1a name hashes, so unrelated CONTROLLED
 * variables in different object files never share a slot. The variable count
 * is tiny, so a linear scan beats a hash table. */
static CtlVar *ctlFind(long long key, int create) {
  for (long long i = 0; i < ctlCount; ++i)
    if (ctlVars[i].key == key)
      return &ctlVars[i];
  if (!create)
    return NULL;
  CtlVar *nv = (CtlVar *)realloc(ctlVars, (size_t)(ctlCount + 1) * sizeof(CtlVar));
  if (!nv)
    pli_signal_error("CONTROLLED table out of memory");
  ctlVars = nv;
  CtlVar *v = &ctlVars[ctlCount++];
  v->key = key;
  v->gens = NULL;
  v->lens = NULL;
  v->ranks = NULL;
  v->dimss = NULL;
  v->depth = 0;
  v->cap = 0;
  return v;
}

/* Grow a generation stack's parallel arrays to hold one more generation. */
static void ctlGrow(CtlVar *v) {
  if (v->depth < v->cap)
    return;
  long long ncap = v->cap ? v->cap * 2 : 4;
  char **ng = (char **)realloc(v->gens, (size_t)ncap * sizeof(char *));
  if (!ng)
    pli_signal_error("CONTROLLED generation stack out of memory");
  long long *nl = (long long *)realloc(v->lens, (size_t)ncap * sizeof(long long));
  if (!nl)
    pli_signal_error("CONTROLLED generation stack out of memory");
  long long *nr = (long long *)realloc(v->ranks, (size_t)ncap * sizeof(long long));
  if (!nr)
    pli_signal_error("CONTROLLED generation stack out of memory");
  long long **nd = (long long **)realloc(v->dimss, (size_t)ncap * sizeof(long long *));
  if (!nd)
    pli_signal_error("CONTROLLED generation stack out of memory");
  v->gens = ng;
  v->lens = nl;
  v->ranks = nr;
  v->dimss = nd;
  v->cap = ncap;
}

void pli_ctl_alloc(long long key, long long n) {
  CtlVar *v = ctlFind(key, 1);
  ctlGrow(v);
  v->gens[v->depth] = pli_alloc(n);
  v->lens[v->depth] = n;
  v->ranks[v->depth] = 0;
  v->dimss[v->depth] = NULL;
  ++v->depth;
}

/* Push an N-D generation (rules (13),(89)): total payload bytes plus room for
 * rank per-axis extents, filled by pli_ctl_set_dim after the call. */
void pli_ctl_alloc_dims(long long key, long long n, long long rank) {
  CtlVar *v = ctlFind(key, 1);
  if (rank < 0)
    rank = 0;
  ctlGrow(v);
  v->gens[v->depth] = pli_alloc(n);
  v->lens[v->depth] = n;
  v->ranks[v->depth] = rank;
  v->dimss[v->depth] = rank ? (long long *)calloc((size_t)rank, sizeof(long long)) : NULL;
  if (rank && !v->dimss[v->depth])
    pli_signal_error("CONTROLLED generation stack out of memory");
  ++v->depth;
}

/* Record one axis extent of the latest generation (0-based axis). The rank
 * grows to cover the axis, so a plain pli_ctl_alloc generation can gain
 * descriptor extents afterwards (bare ALLOCATE of a fixed N-D array). */
void pli_ctl_set_dim(long long key, long long axis, long long extent) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0 || axis < 0)
    return;
  if (axis >= v->ranks[v->depth - 1]) {
    long long nrank = axis + 1;
    long long *nd = (long long *)realloc(v->dimss[v->depth - 1], (size_t)nrank * sizeof(long long));
    if (!nd)
      pli_signal_error("CONTROLLED generation stack out of memory");
    for (long long k = v->ranks[v->depth - 1]; k < nrank; ++k)
      nd[k] = 0;
    v->dimss[v->depth - 1] = nd;
    v->ranks[v->depth - 1] = nrank;
  }
  v->dimss[v->depth - 1][(size_t)axis] = extent;
}

/* Axis count of the latest generation, 0 when the stack is empty or the top
 * generation carries no per-axis extents (1-D/scalar/CHAR path). */
long long pli_ctl_rank(long long key) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0)
    return 0;
  return v->ranks[v->depth - 1];
}

/* Extent of one axis (0-based) of the latest generation, 0 when absent. */
long long pli_ctl_extent(long long key, long long axis) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0)
    return 0;
  if (axis < 0 || axis >= v->ranks[v->depth - 1])
    return 0;
  return v->dimss[v->depth - 1][(size_t)axis];
}

/* Generation-stack depth: the number of live generations (for ALLOCATE (*)
 * reuse, which needs a previous generation to copy from). */
long long pli_ctl_depth(long long key) {
  CtlVar *v = ctlFind(key, 0);
  if (!v)
    return 0;
  return v->depth;
}

void pli_ctl_free(long long key) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0)
    pli_signal_error("FREE of empty CONTROLLED stack");
  --v->depth;
  free(v->gens[v->depth]);
  free(v->dimss[v->depth]);
  v->dimss[v->depth] = NULL;
}

char *pli_ctl_addr(long long key) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0)
    return NULL;
  return v->gens[v->depth - 1];
}

/* Ensure a current generation exists (rules (15),(87)): the first store to a
 * CHAR(*) CONTROLLED variable implicitly allocates one of the needed size. */
void pli_ctl_ensure(long long key, long long n) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0)
    pli_ctl_alloc(key, n);
}

long long pli_ctl_len(long long key) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0)
    return 0;
  return v->lens[v->depth - 1];
}

/* AREA (rule (20)): a region for out-of-order BASED allocation. The region
 * tracks the bytes currently handed out and refuses to exceed its declared
 * size (the AREA condition). Each block carries a 16-byte header holding its
 * payload size and the next live block, so FREE returns the right amount in
 * any order and destroying the region releases whatever is still allocated;
 * the payload stays aligned for any PL/I value. */
typedef struct PliBlock {
  long long size; /* payload bytes */
  struct PliBlock *next;
} PliBlock;

typedef struct {
  long long size;   /* region capacity in bytes */
  long long used;   /* bytes currently allocated from the region */
  PliBlock *blocks; /* live blocks, newest first */
} PliArea;

char *pli_area_create(long long n) {
  PliArea *a = (PliArea *)malloc(sizeof(PliArea));
  if (!a)
    pli_abort("AREA: out of memory");
  a->size = n > 0 ? n : 0;
  a->used = 0;
  a->blocks = NULL;
  return (char *)a;
}

void pli_area_destroy(char *ap) {
  PliArea *a = (PliArea *)ap;
  if (!a)
    return;
  for (PliBlock *b = a->blocks; b;) {
    PliBlock *next = b->next;
    free(b);
    b = next;
  }
  free(a);
}

char *pli_area_alloc(char *ap, long long n) {
  PliArea *a = (PliArea *)ap;
  if (!a)
    pli_signal_error("AREA: allocation from a null area");
  long long need = n < 0 ? 0 : n;
  if (a->used + need > a->size)
    pli_signal_error("AREA: area overflow");
  PliBlock *b = (PliBlock *)malloc((size_t)need + sizeof(PliBlock));
  if (!b)
    pli_abort("AREA: out of memory");
  b->size = need;
  b->next = a->blocks;
  a->blocks = b;
  a->used += need;
  return (char *)(b + 1);
}

void pli_area_free(char *ap, char *p) {
  if (!p)
    return;
  PliArea *a = (PliArea *)ap;
  PliBlock *b = (PliBlock *)p - 1;
  if (a) {
    /* Unlink the block from the region's live list. */
    PliBlock **link = &a->blocks;
    while (*link && *link != b)
      link = &(*link)->next;
    if (*link)
      *link = b->next;
    a->used -= b->size;
  }
  free(b);
}
