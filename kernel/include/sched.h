#pragma once

#include <stdint.h>

/* Kernel threads and a preemptive round-robin scheduler. One CPU. */

#define KSTACK_SIZE   (16 * 1024)
#define SCHED_QUANTUM 5 /* timer ticks a thread runs before preemption */

enum thread_state { T_RUNNABLE, T_RUNNING, T_DEAD };

struct thread {
  uint64_t rsp; /* saved by switch_to; must stay first (sched.asm uses 0) */
  uint8_t *kstack; /* kmalloc'd stack, NULL for the boot thread */
  enum thread_state state;
  int tid;
  const char *name;
  struct thread *next; /* circular run list */
};

/* Make the code running now (kmain_late, on boot_stack) thread 0. Call
 * before timer_init(). */
void sched_init(void);

/* Start `fn(arg)` in a new thread, run after every thread already in the
 * list. Returning from `fn` exits the thread. NULL if out of memory. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

/* Give up the CPU to the next runnable thread. */
void thread_yield(void);

/* End the calling thread. Its stack is freed later by sched_reap(). */
__attribute__((noreturn)) void thread_exit(void);

struct thread *thread_current(void);

/* Threads that have not exited, including the caller. */
int sched_nthreads(void);

/* Free exited threads. Thread context only: it calls kfree, which an
 * interrupt handler must not (the allocator has no lock). */
void sched_reap(void);

/* Called from the timer interrupt, after the EOI. */
void sched_tick(void);
