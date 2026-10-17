#include "syscall.h"

#include <abi/errno.h>
#include <abi/syscall.h>
#include <abi/wait.h>
#include <idt.h>
#include <kprintf.h>
#include <path.h>
#include <proc.h>
#include <sched.h>
#include <serial.h>
#include <slab.h>
#include <stddef.h>
#include <usercopy.h>
#include <vmm.h>

/*
 * The capability a syscall needs. Every table entry must name one:
 * dispatch() refuses an entry left at CAP_UNDECLARED, so forgetting to
 * think about a new syscall's permission fails closed.
 */
enum cap {
  CAP_UNDECLARED = 0,
  CAP_ANY, /* every process may make this call */
};

/* `f` is the caller's saved registers, for the calls that replace them
 * (fork copies them, exec rewrites them). Most ignore it. */
typedef int64_t (*syscall_fn)(const uint64_t arg[6], struct trap_frame *f);

struct syscall {
  syscall_fn fn;
  enum cap cap;
  const char *name;
};

static int64_t sys_exit(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  thread_exit(W_EXITCODE((int)arg[0]));
}

static int64_t sys_getpid(const uint64_t arg[6], struct trap_frame *f) {
  (void)arg, (void)f;
  return thread_current()->tid;
}

/* The child gets a copy of everything: memory, registers (with rax 0, so
 * fork returns 0 there), and the parent's name. */
static int64_t sys_fork(const uint64_t arg[6], struct trap_frame *f) {
  (void)arg;
  struct thread *parent = thread_current();
  uint64_t pml4 = vmm_space_new();
  if (pml4 == 0)
    return -ENOMEM;
  if (vmm_space_copy(pml4, parent->pml4) != 0) {
    vmm_space_free(pml4);
    return -ENOMEM;
  }
  struct trap_frame tf = *f;
  tf.rax = 0;
  /* Interrupts are off (interrupt gate): the child can't run before we
   * read its tid. */
  struct thread *child = thread_create_user(parent->name, pml4, &tf, parent);
  return child != NULL ? child->tid : -ENOMEM;
}

/* exec(path, argv): argv is a NULL-terminated array of strings, or NULL. */
static int64_t sys_exec(const uint64_t arg[6], struct trap_frame *f) {
  char path[PATH_MAX], norm[PATH_MAX];
  int n = copy_str_from_user(path, (const char *)arg[0], sizeof path);
  if (n < 0)
    return n;
  int err = path_normalize(path, norm, sizeof norm);
  if (err != 0)
    return err == -EINVAL ? -ENOENT : err;

  /* The strings, packed into one page; at most a page reaches the stack. */
  char *strings = kmalloc(PAGE_SIZE);
  if (strings == NULL)
    return -ENOMEM;
  const char *argv[PROC_MAX_ARGS];
  const uint64_t *uargv = (const uint64_t *)arg[1];
  int argc = 0;
  size_t used = 0;
  while (uargv != NULL) {
    uint64_t up;
    if (copy_from_user(&up, uargv + argc, sizeof up) != 0) {
      err = -EFAULT;
      break;
    }
    if (up == 0)
      break;
    if (argc == PROC_MAX_ARGS) {
      err = -E2BIG;
      break;
    }
    n = copy_str_from_user(strings + used, (const char *)up, PAGE_SIZE - used);
    if (n < 0) {
      err = n == -ENAMETOOLONG ? -E2BIG : n;
      break;
    }
    argv[argc++] = strings + used;
    used += n + 1;
  }
  if (err == 0)
    err = proc_exec(f, norm, argv, argc);
  kfree(strings);
  return err;
}

/* wait(int *status): a child's pid, its status stored if status != NULL. */
static int64_t sys_wait(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  int status;
  int pid = thread_wait(&status);
  if (pid < 0)
    return pid;
  if (arg[0] != 0 && copy_to_user((void *)arg[0], &status, sizeof status) != 0)
    return -EFAULT; /* the child is collected regardless */
  return pid;
}

/* kill(pid): end a process, as if by SIGKILL. */
static int64_t sys_kill(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  struct thread *t = thread_find((int)arg[0]);
  if (t == NULL)
    return -ESRCH;
  if (t->pml4 == 0)
    return -EPERM; /* a kernel thread */
  thread_kill(t, W_SIGNALED(SIGKILL));
  return 0;
}

static int64_t sys_debug_write(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  const char *ubuf = (const char *)arg[0];
  size_t len = arg[1];
  char chunk[64];
  for (size_t done = 0; done < len;) {
    size_t n = len - done < sizeof chunk ? len - done : sizeof chunk;
    if (copy_from_user(chunk, ubuf + done, n) != 0)
      return -EFAULT;
    for (size_t i = 0; i < n; i++)
      serial_putc(COM1, chunk[i]);
    done += n;
  }
  return len;
}

static const struct syscall table[] = {
    [SYS_exit] = {sys_exit, CAP_ANY, "exit"},
    [SYS_debug_write] = {sys_debug_write, CAP_ANY, "debug_write"},
    [SYS_getpid] = {sys_getpid, CAP_ANY, "getpid"},
    [SYS_fork] = {sys_fork, CAP_ANY, "fork"},
    [SYS_exec] = {sys_exec, CAP_ANY, "exec"},
    [SYS_wait] = {sys_wait, CAP_ANY, "wait"},
    [SYS_kill] = {sys_kill, CAP_ANY, "kill"},
};

#define NSYSCALLS (sizeof table / sizeof table[0])

/* Always allows for now; the provenance-label check goes here. */
static int cap_check(enum cap cap) {
  (void)cap;
  return 0;
}

/* The single checkpoint: every syscall enters the kernel through here. */
static void dispatch(struct trap_frame *f) {
  uint64_t nr = f->rax;
  const struct syscall *s = nr < NSYSCALLS ? &table[nr] : NULL;
  if (s == NULL || s->fn == NULL) {
    f->rax = -ENOSYS;
    return;
  }
  if (s->cap == CAP_UNDECLARED) {
    kprintf("syscall: %s declares no capability, refused\n", s->name);
    f->rax = -ENOSYS;
    return;
  }
  int err = cap_check(s->cap);
  if (err != 0) {
    f->rax = err;
    return;
  }

  const uint64_t arg[6] = {f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9};
  f->rax = s->fn(arg, f);
}

void syscall_init(void) { idt_register(SYSCALL_VECTOR, dispatch); }
