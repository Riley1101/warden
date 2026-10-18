#include "t.h"

#define BULK 10000 /* more than a pipe holds: the writer must block */

static char buf[BULK];

int main(void) {
  int fds[2];
  char small[16];

  /* In one end, out the other; EOF once the write end is closed. */
  if (pipe(fds) != 0 || fds[0] != 3 || fds[1] != 4) /* lowest free */
    return 1;
  if (write(fds[1], "hello", 5) != 5)
    return 2;
  if (read(fds[0], small, sizeof small) != 5 || memcmp(small, "hello", 5) != 0)
    return 3;
  if (close(fds[1]) != 0 || read(fds[0], small, sizeof small) != 0)
    return 4;
  close(fds[0]);

  /* No reader left: the write fails instead of blocking forever. */
  pipe(fds);
  close(fds[0]);
  if (!FAILS(write(fds[1], "x", 1), EPIPE))
    return 5;
  close(fds[1]);

  /* Bad descriptors and bad pointers. */
  if (!FAILS(read(99, small, 1), EBADF) || !FAILS(close(99), EBADF) ||
      !FAILS(write(-1, "x", 1), EBADF))
    return 6;
  pipe(fds);
  if (!FAILS(write(fds[0], "x", 1), EBADF) || /* the read end */
      !FAILS(read(fds[1], small, 1), EBADF))  /* the write end */
    return 7;
  const char *volatile null = NULL;
  if (!FAILS(write(fds[1], null, 1), EFAULT))
    return 8;
  close(fds[0]);
  close(fds[1]);
  int *volatile bad = (int *)8;
  if (!FAILS(pipe(bad), EFAULT))
    return 9;
  if (pipe(fds) != 0 || fds[0] != 3) /* the failed pipe left no fds */
    return 10;
  close(fds[0]);
  close(fds[1]);

  /* A child streams more than the pipe holds; we read it all, in order,
   * then EOF when the child exits (its descriptors close with it). */
  pipe(fds);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    for (int i = 0; i < BULK; i++)
      buf[i] = (char)(i % 251);
    _exit(write(fds[1], buf, BULK) == BULK ? 0 : 1);
  }
  close(fds[1]);
  long got = 0;
  ssize_t n;
  while ((n = read(fds[0], buf + got, BULK - got + 1)) > 0)
    got += n;
  if (n != 0 || got != BULK)
    return 11;
  for (int i = 0; i < BULK; i++)
    if (buf[i] != (char)(i % 251))
      return 12;
  int st;
  if (wait(&st) != pid || st != W_EXITCODE(0))
    return 13;
  close(fds[0]);

  /* A killed writer closes its end too: the reader sees EOF. */
  pipe(fds);
  pid = fork();
  if (pid == 0)
    for (;;)
      ; /* holds the write end until killed */
  close(fds[1]);
  kill(pid, SIGKILL);
  if (read(fds[0], small, 1) != 0)
    return 14;
  wait(NULL);
  close(fds[0]);
  return 0;
}
