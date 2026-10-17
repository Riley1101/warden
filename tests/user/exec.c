#include "t.h"

int main(void) {
  char *args[] = {"args", "a", "bc", NULL};

  /* Failures return, and leave this program running. */
  if (!FAILS(execv("/test/nope", args), ENOENT))
    return 1;
  if (!FAILS(execv("/test/notelf", NULL), ENOEXEC))
    return 2;
  if (!FAILS(execv(NULL, NULL), EFAULT))
    return 3;
  char **volatile bad_argv = (char **)8; /* hidden from gcc's checks */
  if (!FAILS(execv("/test/args", bad_argv), EFAULT))
    return 4;
  if (!FAILS(execv("test/args", NULL), ENOENT)) /* relative */
    return 5;

  /* A child becomes /test/args (path normalized on the way). */
  pid_t pid = fork();
  if (pid == 0) {
    execv("/test/./args", args);
    _exit(99);
  }
  int st;
  if (wait(&st) != pid)
    return 6;
  if (!WIFEXITED(st))
    return 7;
  if (WEXITSTATUS(st) != 0)
    return 10 + WEXITSTATUS(st); /* args' own failing step */

  /* And this process becomes /test/hello: whoever waits sees its 7. */
  execv("/test/hello", NULL);
  return 8;
}
