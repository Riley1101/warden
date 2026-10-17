#include "t.h"

/* Fork a child that dies by `how`, and return the status it died with. */
static int child_status(void (*how)(void)) {
  pid_t pid = fork();
  if (pid == 0) {
    how();
    _exit(0);
  }
  int st;
  return wait(&st) == pid ? st : -1;
}

static void null_write(void) { *(volatile int *)0 = 1; }
static void bad_opcode(void) { __asm__ __volatile__("ud2"); }

int main(void) {
  pid_t pid = fork();
  if (pid == 0)
    for (;;)
      ; /* until killed */

  if (kill(pid, SIGKILL) != 0)
    return 1;
  int st;
  if (wait(&st) != pid)
    return 2;
  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL)
    return 3;
  if (!FAILS(kill(pid, SIGKILL), ESRCH)) /* gone */
    return 4;
  if (!FAILS(kill(0, SIGKILL), EPERM)) /* the boot thread: not a process */
    return 5;
  if (!FAILS(kill(getpid(), SIGSEGV), EINVAL)) /* only SIGKILL exists */
    return 8;

  /* A fault kills only the child, and says why. */
  st = child_status(null_write);
  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV)
    return 6;
  st = child_status(bad_opcode);
  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGILL)
    return 7;
  return 0;
}
