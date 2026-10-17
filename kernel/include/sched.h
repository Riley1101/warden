#pragma once

#include <file.h>
#include <idt.h>
#include <stdint.h>

/*
 * Threads and a preemptive round-robin scheduler. One CPU.
 *
 * A user thread is a process: one thread, one address space, and its pid
 * is its tid. A process with a parent becomes a zombie when it dies, until
 * the parent collects its status with thread_wait(); one without (made by
 * the kernel, or orphaned) is freed as soon as it dies.
 */

#define KSTACK_SIZE   (16 * 1024)
#define SCHED_QUANTUM 5 /* timer ticks a thread runs before preemption */
#define THREAD_NAME   32

enum thread_state {
  T_RUNNABLE,
  T_RUNNING,
  T_SLEEPING, /* in thread_sleep, until thread_wakeup(wchan) */
  T_ZOMBIE,  /* dead, status not yet collected by the parent */
  T_DEAD,    /* sched_reap frees it */
};

struct thread {
  uint64_t rsp; /* saved by switch_to; must stay first (sched.asm uses 0) */
  uint8_t *kstack; /* kmalloc'd stack, NULL for the boot thread */
  enum thread_state state;
  int tid;
  char name[THREAD_NAME];
  struct thread *next; /* circular run list */
  void *wchan;         /* what it sleeps on, while T_SLEEPING */
  /* User threads only (pml4 == 0 for kernel threads, which run in the
   * kernel address space): */
  uint64_t pml4;          /* address space, freed with the thread */
  uint64_t brk_base, brk; /* heap: [brk_base, brk), whole pages mapped */
  struct thread *parent;  /* who may wait for it; NULL: nobody */
  int exit_status;        /* abi/wait.h encoding, once it is a zombie */
  struct file *fd[NFD];   /* open files; closed when it dies */
};

/* Make the code running now (kmain_late, on boot_stack) thread 0. Call
 * before timer_init(). */
void sched_init(void);

/* Start `fn(arg)` in a new thread, run after every thread already in the
 * list. Returning from `fn` exits the thread. NULL if out of memory. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

/* A trap frame that enters ring 3 at `rip` with stack `rsp`, every other
 * register zero, interrupts on. */
void trap_frame_user(struct trap_frame *tf, uint64_t rip, uint64_t rsp);

/*
 * Start a thread that returns to ring 3 with the registers in `tf`, in
 * address space `pml4`, as a child of `parent` (or NULL). The thread owns
 * `pml4` from here on, even on failure (NULL), and frees it when it dies.
 */
struct thread *thread_create_user(const char *name, uint64_t pml4,
                                  const struct trap_frame *tf,
                                  struct thread *parent);

/* The live (not zombie or dead) thread `tid`, or NULL. */
struct thread *thread_find(int tid);

/* End `t` with `status`. If it is the caller, this is thread_exit(). */
void thread_kill(struct thread *t, int status);

/*
 * Wait for a child of the caller to die. Returns its tid and sets *status,
 * or -ECHILD if the caller has no children. Blocks while they all live.
 */
int thread_wait(int *status);

/*
 * Block until thread_wakeup(chan). Interrupts must be off, and stay off
 * from checking the condition slept on to this call, so no wakeup is
 * missed (one CPU). Wakeups can be spurious: check the condition again.
 */
void thread_sleep(void *chan);

/* Make every thread sleeping on `chan` runnable. Interrupts must be off. */
void thread_wakeup(void *chan);

/* Give up the CPU to the next runnable thread. */
void thread_yield(void);

/* End the calling thread. Its stack is freed later by sched_reap(). */
__attribute__((noreturn)) void thread_exit(int status);

struct thread *thread_current(void);

/* Rename `t` (truncated to THREAD_NAME - 1 bytes). */
void thread_set_name(struct thread *t, const char *name);

/* Threads still alive (not zombie or dead), including the caller. */
int sched_nthreads(void);

/* Free dead threads. Thread context only: it calls kfree, which an
 * interrupt handler must not (the allocator has no lock). */
void sched_reap(void);

/* Called from the timer interrupt, after the EOI. */
void sched_tick(void);
