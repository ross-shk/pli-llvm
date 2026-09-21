/* rt_storage.c — PL/I runtime library (libpli): heap: ALLOCATE/FREE. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>
#include <stdlib.h>

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

