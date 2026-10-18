#include "sched.h"

#include <abi/errno.h>
#include <cpu.h>
#include <file.h>
#include <gdt.h>
#include <kprintf.h>
#include <kstring.h>
#include <slab.h>
#include <vmm.h>

void switch_to(uint64_t *old_rsp, uint64_t new_rsp); /* sched.asm */
void thread_trampoline(void);
void trap_return(void); /* idt.asm: pop a trap_frame and iretq */

/* The boot thread runs on boot_stack; switch_to fills in its rsp. */
static struct thread boot_thread;
static struct thread *current;
static int next_tid;
static int quantum;

void thread_set_name(struct thread *t, const char *name) {
  size_t i = 0;
  for (; name[i] != '\0' && i < THREAD_NAME - 1; i++)
    t->name[i] = name[i];
  t->name[i] = '\0';
}

void sched_init(void) {
  boot_thread = (struct thread){
      .state = T_RUNNING, .tid = 0, .next = &boot_thread};
  thread_set_name(&boot_thread, "boot");
  current = &boot_thread;
  next_tid = 1;
  quantum = SCHED_QUANTUM;
}

struct thread *thread_current(void) { return current; }

/* Set while schedule() halts for want of a runnable thread: the timer must
 * not call back into it. */
static int idling;

/* Round robin: the next runnable thread after `current` in the list, or
 * stay on `current`. Interrupts must be off. */
static void schedule(void) {
  struct thread *old = current;
  struct thread *next;
  for (;;) {
    next = old->next;
    while (next != old && next->state != T_RUNNABLE)
      next = next->next;
    if (next != old || old->state == T_RUNNING || old->state == T_RUNNABLE)
      break;
    if (old->state == T_DEAD || old->state == T_ZOMBIE)
      panic("sched: last thread exited"); /* boot never exits */
    /* Everyone sleeps: halt, with interrupts on, until one of them is
     * woken (by an interrupt handler), then look again. */
    idling = 1;
    __asm__ __volatile__("sti\n\thlt\n\tcli" : : : "memory");
    idling = 0;
  }

  quantum = SCHED_QUANTUM;
  if (next == old) {
    old->state = T_RUNNING;
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
  if (current == NULL || idling)
    return;
  if (--quantum <= 0)
    schedule();
}

void thread_yield(void) {
  uint64_t flags = irq_save();
  schedule();
  irq_restore(flags);
}

void thread_sleep(void *chan) {
  current->state = T_SLEEPING;
  current->wchan = chan;
  schedule();
}

void thread_wakeup(void *chan) {
  struct thread *t = current;
  do {
    if (t->state == T_SLEEPING && t->wchan == chan) {
      t->state = T_RUNNABLE;
      t->wchan = NULL;
    }
    t = t->next;
  } while (t != current);
}

/*
 * `t` is dead with `status`. Its files are closed (a pipe's other end sees
 * EOF), its children orphaned (a zombie child is freed: nobody will
 * collect it now). With a parent it stays a zombie, and wakes the parent
 * if it is waiting. Interrupts must be off.
 */
static void die(struct thread *t, int status) {
  fd_close_all(t->fd);
  for (struct thread *c = t->next; c != t; c = c->next) {
    if (c->parent != t)
      continue;
    c->parent = NULL;
    if (c->state == T_ZOMBIE)
      c->state = T_DEAD;
  }
  t->exit_status = status;
  if (t->parent == NULL) {
    t->state = T_DEAD;
    return;
  }
  t->state = T_ZOMBIE;
  thread_wakeup(t->parent); /* in thread_wait */
}

void thread_exit(int status) {
  irq_save();
  die(current, status);
  schedule();
  __builtin_unreachable();
}

/* A thread with a stack but not yet on the run list. */
static struct thread *alloc_thread(const char *name) {
  sched_reap();

  struct thread *t = kzalloc(sizeof *t);
  uint8_t *stack = kmalloc(KSTACK_SIZE);
  if (t == NULL || stack == NULL) {
    kfree(t);
    kfree(stack);
    return NULL;
  }
  t->kstack = stack;
  t->state = T_RUNNABLE;
  thread_set_name(t, name);
  return t;
}

/* Below `sp`, a frame as if the thread had called switch_to: popped into
 * the callee-saved registers, then `ret` to `entry`. */
static uint64_t *push_switch_frame(uint64_t *sp, void (*entry)(void),
                                   uint64_t rbx, uint64_t r12) {
  *--sp = (uint64_t)entry;
  *--sp = 0;   /* rbp */
  *--sp = rbx; /* rbx */
  *--sp = r12; /* r12 */
  *--sp = 0;   /* r13 */
  *--sp = 0;   /* r14 */
  *--sp = 0;   /* r15 */
  return sp;
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
  struct thread *t = alloc_thread(name);
  if (t == NULL)
    return NULL;
  /* The trampoline calls fn(arg); two zero words on top leave RSP 16-byte
   * aligned at that call. */
  uint64_t *sp = (uint64_t *)(t->kstack + KSTACK_SIZE);
  *--sp = 0;
  *--sp = 0;
  sp = push_switch_frame(sp, thread_trampoline, (uint64_t)fn, (uint64_t)arg);
  t->rsp = (uint64_t)sp;
  start_thread(t);
  return t;
}

void trap_frame_user(struct trap_frame *tf, uint64_t rip, uint64_t rsp) {
  *tf = (struct trap_frame){
      .rip = rip,
      .cs = GDT_UCODE,
      .rflags = RFLAGS_IF | 0x2, /* bit 1 is always set */
      .rsp = rsp,
      .ss = GDT_UDATA,
  };
}

struct thread *thread_create_user(const char *name, uint64_t pml4,
                                  const struct trap_frame *tf,
                                  struct thread *parent) {
  struct thread *t = alloc_thread(name);
  if (t == NULL) {
    vmm_space_free(pml4);
    return NULL;
  }
  t->pml4 = pml4;
  t->parent = parent;
  /* The frame at the top of the stack, where an interrupt from ring 3
   * would have left it; switch_to's `ret` lands in trap_return, which pops
   * it into the registers and irets. */
  struct trap_frame *f = (struct trap_frame *)(t->kstack + KSTACK_SIZE) - 1;
  *f = *tf;
  t->rsp = (uint64_t)push_switch_frame((uint64_t *)f, trap_return, 0, 0);
  start_thread(t);
  return t;
}

struct thread *thread_find(int tid) {
  uint64_t flags = irq_save();
  struct thread *t = current, *found = NULL;
  do {
    if (t->tid == tid && t->state != T_ZOMBIE && t->state != T_DEAD)
      found = t;
    t = t->next;
  } while (t != current && found == NULL);
  irq_restore(flags);
  return found;
}

/* A thread that isn't running is parked in switch_to, so marking it dead
 * is enough: it never runs again, and sched_reap frees it. */
void thread_kill(struct thread *t, int status) {
  if (t == current)
    thread_exit(status);
  uint64_t flags = irq_save();
  if (t->state != T_ZOMBIE && t->state != T_DEAD)
    die(t, status);
  irq_restore(flags);
}

int thread_wait(int *status) {
  uint64_t flags = irq_save();
  for (;;) {
    int children = 0;
    for (struct thread *c = current->next; c != current; c = c->next) {
      if (c->parent != current)
        continue;
      children++;
      if (c->state == T_ZOMBIE) {
        int tid = c->tid;
        *status = c->exit_status;
        c->parent = NULL;
        c->state = T_DEAD;
        irq_restore(flags);
        return tid;
      }
    }
    if (children == 0) {
      irq_restore(flags);
      return -ECHILD;
    }
    thread_sleep(current); /* a child's die() wakes us */
  }
}

int sched_nthreads(void) {
  uint64_t flags = irq_save();
  int n = 0;
  struct thread *t = current;
  do {
    if (t->state != T_DEAD && t->state != T_ZOMBIE)
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
