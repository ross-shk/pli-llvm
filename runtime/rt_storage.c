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
  v->depth = 0;
  v->cap = 0;
  return v;
}

void pli_ctl_alloc(long long key, long long n) {
  CtlVar *v = ctlFind(key, 1);
  if (v->depth >= v->cap) {
    long long ncap = v->cap ? v->cap * 2 : 4;
    char **ng = (char **)realloc(v->gens, (size_t)ncap * sizeof(char *));
    if (!ng)
      pli_signal_error("CONTROLLED generation stack out of memory");
    long long *nl = (long long *)realloc(v->lens, (size_t)ncap * sizeof(long long));
    if (!nl)
      pli_signal_error("CONTROLLED generation stack out of memory");
    v->gens = ng;
    v->lens = nl;
    v->cap = ncap;
  }
  v->gens[v->depth] = pli_alloc(n);
  v->lens[v->depth] = n;
  ++v->depth;
}

void pli_ctl_free(long long key) {
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0)
    pli_signal_error("FREE of empty CONTROLLED stack");
  free(v->gens[--v->depth]);
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
