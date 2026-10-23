#include "util.h"

#include <sys/proc.h>

static char state_letter(unsigned state) {
  switch (state) {
  case P_RUNNING:
    return 'R';
  case P_RUNNABLE:
    return 'r';
  case P_SLEEPING:
    return 'S';
  case P_ZOMBIE:
    return 'Z';
  default:
    return '?';
  }
}

/* ps: every process, the kernel's own threads marked with [brackets]. */
int main(void) {
  struct procinfo p[64];
  int n = ps(p, sizeof p / sizeof p[0]);
  if (n < 0) {
    complain("ps", "ps", errno);
    return 1;
  }
  printf("%5s %5s %s %s\n", "PID", "PPID", "S", "NAME");
  for (int i = 0; i < n; i++)
    printf("%5d %5d %c %s%s%s\n", p[i].pid, p[i].ppid, state_letter(p[i].state),
           p[i].kernel ? "[" : "", p[i].name, p[i].kernel ? "]" : "");
  return 0;
}
