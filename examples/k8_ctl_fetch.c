/* micro: the cost of one pli_ctl_addr fetch — the descriptor lookup that
   K8 (a constant-extent CONTROLLED variable bypasses the runtime
   descriptor) would remove. The key is unknown, so the scan runs its
   course and returns NULL. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
extern char *pli_ctl_addr(long long key);
/* the runtime's abort path, stubbed: the fetch never reaches it */
void pli_abort(void) { abort(); }
void pli_signal_error(const char *m) { (void)m; abort(); }
/* the runtime's abort path, stubbed: the fetch never reaches it */
/* the runtime's mutex, stubbed: the fetch does not take it */
int main(void) {
  const long long n = 100000000;
  struct timespec a, b;
  long long acc = 0;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (long long i = 0; i < n; i++) acc += (long long)pli_ctl_addr(12345);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
  printf("acc=%lld  %.3f s for %lld fetches -> %.2f ns per fetch\n", acc, s, n,
         s / (double)n * 1e9);
  return 0;
}
