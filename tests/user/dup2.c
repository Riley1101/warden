#include "t.h"

/* Fork a child that runs `path` with stdin/stdout replaced by `in`/`out`
 * (-1: keep), closing both pipe ends `p` first. */
static pid_t spawn(const char *path, char **argv, int in, int out, int p[2]) {
  pid_t pid = fork();
  if (pid != 0)
    return pid;
  if (in >= 0)
    dup2(in, STDIN_FILENO);
  if (out >= 0)
    dup2(out, STDOUT_FILENO);
  close(p[0]);
  close(p[1]);
  execv(path, argv);
  _exit(99);
}

int main(void) {
  if (dup2(1, 1) != 1)
    return 1;
  if (!FAILS(dup2(99, 3), EBADF) || !FAILS(dup2(0, 16), EBADF) ||
      !FAILS(dup2(0, -1), EBADF))
    return 2;

  /* dup2 shares the file: writes through either fd land in one pipe. */
  int p[2];
  char buf[32];
  pipe(p);
  if (dup2(p[1], 7) != 7)
    return 3;
  write(p[1], "a", 1);
  write(7, "b", 1);
  close(p[1]);
  close(7); /* the last write end: now EOF */
  if (read(p[0], buf, sizeof buf) != 2 || memcmp(buf, "ab", 2) != 0)
    return 4;
  if (read(p[0], buf, sizeof buf) != 0)
    return 5;
  close(p[0]);

  /* echo's stdout into a pipe, through exec. */
  char *echo[] = {"echo", "piped", "text", NULL};
  pipe(p);
  pid_t pid = spawn("/test/echo", echo, -1, p[1], p);
  close(p[1]);
  long got = 0;
  ssize_t n;
  while ((n = read(p[0], buf + got, sizeof buf - got)) > 0)
    got += n;
  close(p[0]);
  int st;
  if (wait(&st) != pid || st != W_EXITCODE(0))
    return 6;
  if (got != 11 || memcmp(buf, "piped text\n", 11) != 0)
    return 7;

  /* A shell pipeline: echo hello | count (count exits with 6 bytes). */
  char *hello[] = {"echo", "hello", NULL};
  char *count[] = {"count", NULL};
  pipe(p);
  pid_t a = spawn("/test/echo", hello, -1, p[1], p);
  pid_t b = spawn("/test/count", count, p[0], -1, p);
  close(p[0]);
  close(p[1]);
  int sa = -1, sb = -1;
  for (int i = 0; i < 2; i++) {
    pid_t w = wait(&st);
    if (w == a)
      sa = st;
    else if (w == b)
      sb = st;
  }
  if (sa != W_EXITCODE(0))
    return 9;
  if (sb != W_EXITCODE(6))
    return 10;
  return 0;
}
