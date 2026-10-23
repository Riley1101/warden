#pragma once

#include <stdint.h>

/*
 * What ps() reports about one process. A kernel thread has ppid 0 and no
 * address space of its own; dead processes whose parent hasn't collected
 * them yet show as P_ZOMBIE.
 */

#define PROCINFO_NAME 32

#define P_RUNNING  0
#define P_RUNNABLE 1
#define P_SLEEPING 2
#define P_ZOMBIE   3

struct procinfo {
  int32_t pid;
  int32_t ppid;
  uint32_t state;
  uint32_t kernel; /* 1: a kernel thread, not a user process */
  char name[PROCINFO_NAME];
};
