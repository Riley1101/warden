#include "lock.h"

#include <cpu.h>
#include <kprintf.h>

void lock_acquire(struct lock *l) {
  uint64_t flags = irq_save();
  if (l->held)
    panic("lock: %s acquired while held", l->name);
  l->held = 1;
  l->flags = flags;
}

void lock_release(struct lock *l) {
  if (!l->held)
    panic("lock: %s released while free", l->name);
  uint64_t flags = l->flags;
  l->held = 0;
  irq_restore(flags);
}
