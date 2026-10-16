#pragma once

#include <stdint.h>

/*
 * A lock for data that more than one thread touches. One CPU for now, so
 * acquiring it just disables interrupts (no preemption, no handler can
 * run); the old interrupt state is kept in the lock for lock_release.
 *
 * Not recursive: acquiring a lock you hold, or releasing one you don't,
 * panics. Different locks nest; take them in a fixed order:
 *   slab -> vmm -> pmm
 *
 * kernel/lock.c is the real one; host tests link tests/host/lock_stub.c,
 * which keeps the misuse checks, so pmm.c and slab.c stay hardware-free.
 */
struct lock {
  const char *name;
  int held;
  uint64_t flags; /* RFLAGS from before lock_acquire */
};

#define LOCK_INIT(n) {.name = (n)}

void lock_acquire(struct lock *l);
void lock_release(struct lock *l);
