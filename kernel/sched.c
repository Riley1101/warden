#include "sched.h"

#include <cpu.h>
#include <kprintf.h>
#include <slab.h>

void switch_to(uint64_t *old_rsp, uint64_t new_rsp); /* sched.asm */
void thread_trampoline(void);

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

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg) {
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

  /* Append: insert just before `current`, so it runs last in this round. */
  uint64_t flags = irq_save();
  t->tid = next_tid++;
  struct thread *prev = current;
  while (prev->next != current)
    prev = prev->next;
  t->next = current;
  prev->next = t;
  irq_restore(flags);
  return t;
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
