#include "t.h"

static volatile int shared = 1;

int main(void) {
  pid_t me = getpid();
  if (me <= 0)
    return 1;

  /* The child has its own pid and its own copy of memory. */
  pid_t pid = fork();
  if (pid < 0)
    return 2;
  if (pid == 0) {
    if (getpid() == me)
      _exit(100);
    shared = 2;
    _exit(42);
  }
  int st = 0;
  if (wait(&st) != pid)
    return 3;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 42)
    return 4;
  if (shared != 1)
    return 5;
  if (!FAILS(wait(&st), ECHILD))
    return 6;

  /* Several children, collected in whatever order they die. */
  int sum = 0;
  for (int i = 1; i <= 5; i++)
    if (fork() == 0)
      _exit(i);
  for (int i = 0; i < 5; i++) {
    if (wait(&st) <= 0)
      return 7;
    sum += WEXITSTATUS(st);
  }
  if (sum != 15 || !FAILS(wait(NULL), ECHILD))
    return 8;

  /* A bad status pointer: EFAULT, and the child is still collected. */
  if (fork() == 0)
    _exit(0);
  int *volatile bad = (int *)8;
  if (!FAILS(wait(bad), EFAULT) || !FAILS(wait(NULL), ECHILD))
    return 9;

  /* The heap is copied too: the child's malloc'd data is its own. */
  char *heap = strdup("parent");
  pid = fork();
  if (pid == 0) {
    strcpy(heap, "child");
    _exit(strcmp(heap, "child") == 0 ? 0 : 1);
  }
  if (wait(&st) != pid || st != W_EXITCODE(0) || strcmp(heap, "parent") != 0)
    return 10;
  free(heap);
  return 0;
}
