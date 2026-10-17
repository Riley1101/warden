#include "t.h"

int main(void) {
  char *args[] = {"args", "a", "bc", NULL};

  /* Failures return, and leave this program running. */
  if (sys_exec("/test/nope", args) != -ENOENT)
    return 1;
  if (sys_exec("/test/notelf", NULL) != -ENOEXEC)
    return 2;
  if (sys_exec(NULL, NULL) != -EFAULT)
    return 3;
  char **volatile bad_argv = (char **)8; /* hidden from gcc's checks */
  if (sys_exec("/test/args", bad_argv) != -EFAULT)
    return 4;
  if (sys_exec("test/args", NULL) != -ENOENT) /* relative */
    return 5;

  /* A child becomes /test/args (path normalized on the way). */
  long pid = sys_fork();
  if (pid == 0) {
    sys_exec("/test/./args", args);
    sys_exit(99);
  }
  int st;
  if (sys_wait(&st) != pid)
    return 6;
  if (!WIFEXITED(st))
    return 7;
  if (WEXITSTATUS(st) != 0)
    return 10 + WEXITSTATUS(st); /* args' own failing step */

  /* And this process becomes /test/hello: whoever waits sees its 7. */
  sys_exec("/test/hello", NULL);
  return 8;
}
