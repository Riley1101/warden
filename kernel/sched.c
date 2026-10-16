#include "sched.h"

#include <cpu.h>
#include <gdt.h>
#include <kprintf.h>
#include <slab.h>
#include <vmm.h>

void switch_to(uint64_t *old_rsp, uint64_t new_rsp); /* sched.asm */
void thread_trampoline(void);
__attribute__((noreturn)) void enter_user(uint64_t rip, uint64_t rsp);

/* The boot thread runs on boot_stack; switch_to fills in its rsp. */
static struct thread boot_thread;
static struct thread *current;
static int next_tid;
static int quantum;

void sched_init(void) {
  boot_thread = (struct thread){
      .state = T_RUNNING, .tid = 0, .name = "boot", .next = &boot_thread};
  current = &boot_thread;
  next_tid = 1;
  quantum = SCHED_QUANTUM;
}

struct thread *thread_current(void) { return current; }

/* Round robin: the next runnable thread after `current` in the list, or
 * stay on `current`. Interrupts must be off. */
static void schedule(void) {
  struct thread *old = current;
  struct thread *next = old->next;
  while (next != old && next->state != T_RUNNABLE)
    next = next->next;

  quantum = SCHED_QUANTUM;
  if (next == old) {
    if (old->state == T_DEAD)
      panic("sched: last thread exited"); /* boot never exits */
    return;
  }

  if (old->state == T_RUNNING)
    old->state = T_RUNNABLE;
  next->state = T_RUNNING;
  current = next;
  /* An interrupt in ring 3 lands on rsp0. The boot thread never enters
   * ring 3, so it has no stack of ours to point at. */
  if (next->kstack != NULL)
    tss_set_rsp0((uint64_t)(next->kstack + KSTACK_SIZE));
  vmm_switch(next->pml4 ? next->pml4 : vmm_kernel_pml4());
  switch_to(&old->rsp, next->rsp);
}

void sched_tick(void) {
  if (current == NULL)
    return;
  if (--quantum <= 0)
    schedule();
}

void thread_yield(void) {
  uint64_t flags = irq_save();
  schedule();
  irq_restore(flags);
}

void thread_exit(void) {
  irq_save();
  current->state = T_DEAD;
  schedule();
  __builtin_unreachable();
}

/* A thread that runs fn(arg), not yet on the run list. */
static struct thread *new_thread(const char *name, void (*fn)(void *),
                                 void *arg) {
  sched_reap();

  struct thread *t = kzalloc(sizeof *t);
  uint8_t *stack = kmalloc(KSTACK_SIZE);
  if (t == NULL || stack == NULL) {
    kfree(t);
    kfree(stack);
    return NULL;
  }

  /* A frame as if the thread had called switch_to: popped into the
   * callee-saved registers, then `ret` to the trampoline. Two zero words
   * on top leave RSP 16-byte aligned at the trampoline's call. */
  uint64_t *sp = (uint64_t *)(stack + KSTACK_SIZE);
  *--sp = 0;
  *--sp = 0;
  *--sp = (uint64_t)thread_trampoline;
  *--sp = 0;             /* rbp */
  *--sp = (uint64_t)fn;  /* rbx */
  *--sp = (uint64_t)arg; /* r12 */
  *--sp = 0;             /* r13 */
  *--sp = 0;             /* r14 */
  *--sp = 0;             /* r15 */

  t->rsp = (uint64_t)sp;
  t->kstack = stack;
  t->state = T_RUNNABLE;
  t->name = name;
  return t;
}

static void start_thread(struct thread *t) {
  /* Append: insert just before `current`, so it runs last in this round. */
  uint64_t flags = irq_save();
  t->tid = next_tid++;
  struct thread *prev = current;
  while (prev->next != current)
    prev = prev->next;
  t->next = current;
  prev->next = t;
  irq_restore(flags);
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg) {
  struct thread *t = new_thread(name, fn, arg);
  if (t != NULL)
    start_thread(t);
  return t;
}

/* A user thread's first code, in the kernel: schedule() already loaded
 * its address space and rsp0. */
static void user_start(void *arg) {
  (void)arg;
  enter_user(current->user_rip, current->user_rsp);
}

struct thread *thread_create_user(const char *name, uint64_t pml4,
                                  uint64_t rip, uint64_t rsp) {
  struct thread *t = new_thread(name, user_start, NULL);
  if (t == NULL) {
    vmm_space_free(pml4);
    return NULL;
  }
  t->pml4 = pml4;
  t->user_rip = rip;
  t->user_rsp = rsp;
  start_thread(t);
  return t;
}

/* A thread that isn't running is parked in switch_to, so marking it dead
 * is enough: it never runs again, and sched_reap frees it. */
void thread_kill(struct thread *t) {
  if (t == current)
    thread_exit();
  uint64_t flags = irq_save();
  t->state = T_DEAD;
  irq_restore(flags);
}

int sched_nthreads(void) {
  uint64_t flags = irq_save();
  int n = 0;
  struct thread *t = current;
  do {
    if (t->state != T_DEAD)
      n++;
    t = t->next;
  } while (t != current);
  irq_restore(flags);
  return n;
}

/* A dead thread is never `current` here (we are running), so its stack is
 * no longer in use: switch_to finished with it when it switched away. */
void sched_reap(void) {
  uint64_t flags = irq_save();
  struct thread *prev = current;
  struct thread *t = current->next;
  while (t != current) {
    if (t->state == T_DEAD) {
      prev->next = t->next;
      if (t->pml4 != 0)
        vmm_space_free(t->pml4); /* not in CR3: we are running, it isn't */
      kfree(t->kstack);
      kfree(t);
      t = prev->next;
    } else {
      prev = t;
      t = t->next;
    }
  }
  irq_restore(flags);
}
