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
  /* User threads only (pml4 == 0 for kernel threads, which run in the
   * kernel address space): */
  uint64_t pml4;     /* address space, freed with the thread */
  uint64_t user_rip; /* where it enters ring 3 */
  uint64_t user_rsp;
};

/* Make the code running now (kmain_late, on boot_stack) thread 0. Call
 * before timer_init(). */
void sched_init(void);

/* Start `fn(arg)` in a new thread, run after every thread already in the
 * list. Returning from `fn` exits the thread. NULL if out of memory. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

/*
 * Start a thread that enters ring 3 at `rip` with stack `rsp`, in address
 * space `pml4`. The thread owns `pml4` from here on, even on failure
 * (NULL), and frees it when it dies.
 */
struct thread *thread_create_user(const char *name, uint64_t pml4,
                                  uint64_t rip, uint64_t rsp);

/* Kill `t`. If it is the caller, this is thread_exit(). */
void thread_kill(struct thread *t);

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
