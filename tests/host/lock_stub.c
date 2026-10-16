/* Host build of lock.h: no interrupts to disable, but keep the misuse
 * checks, so a path that forgets lock_release fails on the next acquire. */
#include <lock.h>

#include <stdio.h>
#include <stdlib.h>

void lock_acquire(struct lock *l) {
  if (l->held) {
    fprintf(stderr, "lock: %s acquired while held\n", l->name);
    abort();
  }
  l->held = 1;
}

void lock_release(struct lock *l) {
  if (!l->held) {
    fprintf(stderr, "lock: %s released while free\n", l->name);
    abort();
  }
  l->held = 0;
}
