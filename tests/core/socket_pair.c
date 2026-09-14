/* Lane B3 stub: a Unix-domain socketpair standing in for a real link.
 * The PL/I side reaches it only through ENTRY ... EXTERNAL (ADR-021). */
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int sk_pair(int *a, int *b) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
    return -1;
  *a = fds[0];
  *b = fds[1];
  return 0;
}

void sk_send(int *fd, int *v) {
  (void)write(*fd, v, sizeof *v);
}

int sk_recv(int *fd) {
  int v = -1;
  ssize_t n = read(*fd, &v, sizeof v);
  return n == sizeof v ? v : -2;
}

void sk_close(int *fd) {
  close(*fd);
}
