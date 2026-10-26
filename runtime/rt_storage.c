/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_storage.c — PL/I runtime library (pli_runtime): heap: ALLOCATE/FREE. */
#include "pli_rt.h"
/* The CONTROLLED table is per-task (ADR-234): rt_sched.h hands out the copy
 * that belongs to the running task. */
#include "rt_sched.h"
/* Allocation routes through pli_plat_malloc/pli_plat_calloc/pli_plat_realloc
 * (the platform seam), except pli_free itself, whose body names
 * pli_plat_free (the seam free: a pli_free call here would recurse). */

char *pli_alloc(long long n) {
  char *p = (char *)pli_plat_malloc((size_t)(n < 0 ? 0 : n));
  if (!p)
    pli_abort("ALLOCATION: out of memory");
  return p;
}

void pli_free(char *p) { pli_plat_free(p); }

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
/* Per-task (ADR-016's multitasking design, ADR-234): each task owns its
 * CONTROLLED generation stacks, mirroring the per-task ON-handler stack
 * in rt_cond.c. Workers calling CALL ... EVENT therefore allocate and free
 * their own generations without racing the main task or each other through
 * this table (see tests/hardening/task_alloc_combo.pli). Under the fiber
 * model the table follows the fiber, not the OS thread. */
static CtlVar *ctlVars(pli_task_local *st) { return (CtlVar *)st->ctl_vars; }
static long long ctlCount(pli_task_local *st) { return st->ctl_count; }

/* Find (or, when create is set, append) the generation stack for a variable.
 * Keys are the sema-assigned FNV-1a name hashes, so unrelated CONTROLLED
 * variables in different object files never share a slot. The variable count
 * is tiny, so a linear scan beats a hash table. */
static CtlVar *ctlFind(long long key, int create) {
  pli_task_local *st = rt_task_local();
  CtlVar *vars = ctlVars(st);
  long long count = ctlCount(st);
  for (long long i = 0; i < count; ++i)
    if (vars[i].key == key)
      return &vars[i];
  if (!create)
    return NULL;
  CtlVar *nv = (CtlVar *)pli_plat_realloc(vars, (size_t)(count + 1) * sizeof(CtlVar));
  if (!nv)
    pli_signal_error("CONTROLLED table out of memory");
  st->ctl_vars = nv;
  st->ctl_count = count + 1;
  CtlVar *v = &nv[count];
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
  char **ng = (char **)pli_plat_realloc(v->gens, (size_t)ncap * sizeof(char *));
  if (!ng)
    pli_signal_error("CONTROLLED generation stack out of memory");
  long long *nl = (long long *)pli_plat_realloc(v->lens, (size_t)ncap * sizeof(long long));
  if (!nl)
    pli_signal_error("CONTROLLED generation stack out of memory");
  long long *nr = (long long *)pli_plat_realloc(v->ranks, (size_t)ncap * sizeof(long long));
  if (!nr)
    pli_signal_error("CONTROLLED generation stack out of memory");
  long long **nd = (long long **)pli_plat_realloc(v->dimss, (size_t)ncap * sizeof(long long *));
  if (!nd)
    pli_signal_error("CONTROLLED generation stack out of memory");
  v->gens = ng;
  v->lens = nl;
  v->ranks = nr;
  v->dimss = nd;
  v->cap = ncap;
}

/* One-slot generation freelist (ADR-214): LIFO allocate/free pairs of an
 * identical size reuse one cached buffer instead of malloc/free churn.
 * Sound because ALLOCATE leaves contents undefined and FREE pops LIFO, so a
 * cached buffer is indistinguishable from fresh memory. One slot bounds
 * retention to a single buffer (reclaimed by the OS at exit). Only 1-D
 * generations (rank 0) enter the cache; the N-D path bypasses it. The cache
 * never aliases a live generation: every stashed buffer came from a popped
 * depth slot and leaves the cache on the next same-shape alloc. The cache
 * is per-task, like the table: a process-global slot would hand one worker's
 * freed buffer to another worker holding the same key. The three fields live
 * in the task's own state (rt_fiber.h), so they follow the fiber. */

// The allocation returns the new generation's address, so the compiler can
// keep it in a slot (WS-K8, ADR-257) instead of fetching it per reference.
char *pli_ctl_alloc(long long key, long long n) {
  pli_task_local *st = rt_task_local();
  CtlVar *v = ctlFind(key, 1);
  ctlGrow(v);
  // intent: same-key/same-size LIFO churn reuses the cached buffer.
  if (st->ctl_cache_bytes && st->ctl_cache_key == key && st->ctl_cache_len == n) {
    v->gens[v->depth] = (char *)st->ctl_cache_bytes;
    st->ctl_cache_bytes = NULL;
  } else {
    v->gens[v->depth] = pli_alloc(n);
  }
  v->lens[v->depth] = n;
  v->ranks[v->depth] = 0;
  v->dimss[v->depth] = NULL;
  ++v->depth;
  return v->gens[v->depth - 1];
}

/* Push an N-D generation (rules (13),(89)): total payload bytes plus room for
 * rank per-axis extents, filled by pli_ctl_set_dim after the call. */
char *pli_ctl_alloc_dims(long long key, long long n, long long rank) {
  CtlVar *v = ctlFind(key, 1);
  if (rank < 0)
    rank = 0;
  ctlGrow(v);
  v->gens[v->depth] = pli_alloc(n);
  v->lens[v->depth] = n;
  v->ranks[v->depth] = rank;
  v->dimss[v->depth] = rank ? (long long *)pli_plat_calloc((size_t)rank, sizeof(long long)) : NULL;
  if (rank && !v->dimss[v->depth])
    pli_signal_error("CONTROLLED generation stack out of memory");
  ++v->depth;
  return v->gens[v->depth - 1];
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
    long long *nd = (long long *)pli_plat_realloc(v->dimss[v->depth - 1], (size_t)nrank * sizeof(long long));
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

/* Free the top generation and return the new top (NULL when the stack is
 * empty). The compiler uses the return value to refresh its cached address,
 * so a FREE costs one runtime call instead of FREE + pli_ctl_addr. */
char *pli_ctl_free_addr(long long key) {
  pli_task_local *st = rt_task_local();
  CtlVar *v = ctlFind(key, 0);
  if (!v || v->depth <= 0) {
    pli_signal_error("FREE of empty CONTROLLED stack");
    return NULL;
  }
  --v->depth;
  pli_free((char *)v->dimss[v->depth]);
  v->dimss[v->depth] = NULL;
  // intent: stash 1-D buffers for the next same-shape ALLOCATE; evict a
  // stale entry first so retention stays one buffer. N-D generations
  // (which may have gained extents via pli_ctl_set_dim) bypass the cache.
  char *buf = v->gens[v->depth];
  v->gens[v->depth] = NULL;
  if (v->ranks[v->depth] == 0) {
    long long n = v->lens[v->depth];
    if (st->ctl_cache_bytes) {
      if (st->ctl_cache_key == key && st->ctl_cache_len == n) {
        pli_free(buf); // slot already serves this shape: drop the duplicate
        return v->depth > 0 ? v->gens[v->depth - 1] : NULL;
      }
      pli_free((char *)st->ctl_cache_bytes); // evict stale entry before stashing
    }
    st->ctl_cache_key = key;
    st->ctl_cache_bytes = buf;
    st->ctl_cache_len = n;
  } else {
    pli_free(buf);
  }
  return v->depth > 0 ? v->gens[v->depth - 1] : NULL;
}

void pli_ctl_free(long long key) { (void)pli_ctl_free_addr(key); }

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
  PliArea *a = (PliArea *)pli_plat_malloc(sizeof(PliArea));
  if (!a)
    pli_abort("AREA: out of memory");
  a->size = n > 0 ? n : 0;
  a->used = 0;
  a->blocks = NULL;
  return (char *)a;
}

/* STATIC AREA (rule (20)): the region is created on first use and lives for the
 * whole program, so blocks allocated by one activation outlive it and a later
 * activation can FREE them. The slot is the declaring module's global. */
char *pli_area_lazy(char *slotp, long long n) {
  char **slot = (char **)slotp; /* the ABI passes the module global's address */
  if (!*slot)
    *slot = pli_area_create(n);
  return *slot;
}

void pli_area_destroy(char *ap) {
  PliArea *a = (PliArea *)ap;
  if (!a)
    return;
  for (PliBlock *b = a->blocks; b;) {
    PliBlock *next = b->next;
    pli_free((char *)b);
    b = next;
  }
  pli_free((char *)a);
}

/* AREA (rule 94; ADR-323): a region operation raises the condition instead of
 * aborting outright. A unit the program established for AREA runs; with none
 * the implicit action is ERROR (message + exit), as before. A unit that
 * resumes leaves the operation with no effect, which the callers report: NULL
 * from ALLOCATE, and the block still live after a refused FREE. */
#define RT_AREA_KEY (-7LL) /* mirrors Stmt::kAreaCondKey */
static int rt_area_raise(const char *msg) {
  char *ctx = NULL;
  char *h = pli_on_top_fn_pair(RT_AREA_KEY, (char *)&ctx);
  if (!h) {
    pli_signal_error(msg);
    return 0;
  }
  /* IRGen's handler takes its capture context as the only argument; the
   * pointer round-trip through a union avoids an object-to-function cast. */
  union {
    char *obj;
    void (*fn)(void *);
  } u;
  u.obj = h;
  u.fn(ctx);
  return 1;
}

char *pli_area_alloc(char *ap, long long n) {
  PliArea *a = (PliArea *)ap;
  if (!a) {
    rt_area_raise("AREA: allocation from a null area");
    return NULL;
  }
  long long need = n < 0 ? 0 : n;
  /* The block header is charged against the region too, and the check is
   * written so it cannot overflow: a->used <= a->size always holds. */
  long long hdr = (long long)sizeof(PliBlock);
  if (need > a->size - a->used - hdr) {
    rt_area_raise("AREA: area overflow");
    return NULL;
  }
  PliBlock *b = (PliBlock *)pli_plat_malloc((size_t)need + sizeof(PliBlock));
  if (!b)
    pli_abort("AREA: out of memory");
  b->size = need;
  b->next = a->blocks;
  a->blocks = b;
  a->used += need + hdr;
  return (char *)(b + 1);
}

/* OFFSET(x, y) (rule (22); ADR-319): convert the POINTER x into an OFFSET
 * relative to the AREA y. An offset holds an absolute address in this
 * implementation (ADR-170), so the conversion is an identity once the
 * pointer is known to address a generation in y; IBM requires exactly that,
 * and the region is the only object that can answer it. A null pointer
 * yields the null offset. */
char *pli_area_offset(char *ap, char *p) {
  if (!p)
    return NULL;
  PliArea *a = (PliArea *)ap;
  if (!a) {
    rt_area_raise("AREA: OFFSET of a pointer with a null area");
    return p;
  }
  PliBlock *b = (PliBlock *)p - 1;
  for (PliBlock *live = a->blocks; live; live = live->next)
    if (live == b)
      return p;
  rt_area_raise("AREA: OFFSET of a pointer not allocated in this area");
  return p;
}

void pli_area_free(char *ap, char *p) {
  if (!p)
    return;
  PliArea *a = (PliArea *)ap;
  PliBlock *b = (PliBlock *)p - 1;
  if (!a) {
    rt_area_raise("AREA: FREE through a null area");
    return;
  }
  /* Unlink the block from the region's live list. A pointer that this
   * region does not own is refused: freeing it would read a header from
   * unrelated memory and leave another region's block dangling. */
  PliBlock **link = &a->blocks;
  while (*link && *link != b)
    link = &(*link)->next;
  if (!*link) {
    rt_area_raise("AREA: FREE of a pointer not allocated in this area");
    return;
  }
  *link = b->next;
  a->used -= b->size + (long long)sizeof(PliBlock);
  pli_free((char *)b);
}
