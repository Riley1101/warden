#include "t.h"

static volatile int shared = 1;

int main(void) {
  long me = sys_getpid();
  if (me <= 0)
    return 1;

  /* The child has its own pid and its own copy of memory. */
  long pid = sys_fork();
  if (pid < 0)
    return 2;
  if (pid == 0) {
    if (sys_getpid() == me)
      sys_exit(100);
    shared = 2;
    sys_exit(42);
  }
  int st = 0;
  if (sys_wait(&st) != pid)
    return 3;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 42)
    return 4;
  if (shared != 1)
    return 5;
  if (sys_wait(&st) != -ECHILD)
    return 6;

  /* Several children, collected in whatever order they die. */
  int sum = 0;
  for (int i = 1; i <= 5; i++)
    if (sys_fork() == 0)
      sys_exit(i);
  for (int i = 0; i < 5; i++) {
    if (sys_wait(&st) <= 0)
      return 7;
    sum += WEXITSTATUS(st);
  }
  if (sum != 15 || sys_wait(NULL) != -ECHILD)
    return 8;

  /* A bad status pointer: -EFAULT, and the child is still collected. */
  if (sys_fork() == 0)
    sys_exit(0);
  if (sys_wait((int *)8) != -EFAULT || sys_wait(NULL) != -ECHILD)
    return 9;
  return 0;
}
