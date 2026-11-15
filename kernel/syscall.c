#include "syscall.h"

#include <abi/errno.h>
#include <abi/proc.h>
#include <abi/syscall.h>
#include <abi/wait.h>
#include <cap.h>
#include <file.h>
#include <fs.h>
#include <idt.h>
#include <kprintf.h>
#include <path.h>
#include <proc.h>
#include <sched.h>
#include <slab.h>
#include <stddef.h>
#include <usercopy.h>
#include <vmm.h>

/* ps() reports at most this many processes per call. */
#define PS_MAX 64

/* `f` is the caller's saved registers, for the calls that replace them
 * (fork copies them, exec rewrites them). Most ignore it. */
typedef int64_t (*syscall_fn)(const uint64_t arg[6], struct trap_frame *f);

/*
 * The capability a syscall needs (cap.h). Every table entry must name
 * one: dispatch() refuses an entry left at CAP_UNDECLARED, so forgetting
 * to think about a new syscall's permission fails closed.
 */
struct syscall {
  syscall_fn fn;
  uint32_t cap;
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
  /* Interrupts are off (interrupt gate): the child can't run before it has
   * its files, nor die before we read its tid. */
  struct thread *child = thread_create_user(parent->name, pml4, &tf, parent);
  if (child == NULL)
    return -ENOMEM;
  fd_copy(child->fd, parent->fd);
  child->brk_base = parent->brk_base; /* the heap's pages were copied too */
  child->brk = parent->brk;
  child->label = parent->label; /* a child is never freer than its parent */
  return child->tid;
}

/*
 * A path argument, into `out` (PATH_MAX bytes), that the caller may use
 * with `need` (CAP_FS_READ, CAP_FS_WRITE, or both). Every path a syscall
 * acts on comes through here, so this is where the scope is held.
 */
static int user_path(char *out, uint64_t uptr, uint32_t need) {
  int n = copy_str_from_user(out, (const char *)uptr, PATH_MAX);
  if (n < 0)
    return n;
  return cap_path(&thread_current()->label, out, need);
}

/* What open's flags ask of the file: reading, writing, or both. */
static uint32_t open_need(int flags) {
  int mode = flags & O_ACCMODE;
  uint32_t need = mode == O_WRONLY ? 0 : CAP_FS_READ;
  if (mode != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
    need |= CAP_FS_WRITE;
  return need;
}

/* open(path, flags): a descriptor. */
static int64_t sys_open(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  char path[PATH_MAX];
  int flags = (int)arg[1];
  int err = user_path(path, arg[0], open_need(flags));
  return err != 0 ? err : fd_open(thread_current()->fd, path, flags);
}

static int64_t sys_mkdir(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  char path[PATH_MAX];
  int err = user_path(path, arg[0], CAP_FS_WRITE);
  return err != 0 ? err : fs_mkdir(path);
}

static int64_t sys_unlink(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  char path[PATH_MAX];
  int err = user_path(path, arg[0], CAP_FS_WRITE);
  return err != 0 ? err : fs_unlink(path);
}

/* Both ends are writes: nothing moves into the scope to be deleted
 * there, nor out of it. */
static int64_t sys_rename(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  char from[PATH_MAX], to[PATH_MAX];
  int err = user_path(from, arg[0], CAP_FS_WRITE);
  if (err == 0)
    err = user_path(to, arg[1], CAP_FS_WRITE);
  return err != 0 ? err : fs_rename(from, to);
}

/* stat(path, struct stat *) */
static int64_t sys_stat(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  char path[PATH_MAX];
  struct stat st;
  int err = user_path(path, arg[0], CAP_FS_READ);
  if (err == 0)
    err = fs_stat(path, &st);
  if (err == 0 && copy_to_user((void *)arg[1], &st, sizeof st) != 0)
    err = -EFAULT;
  return err;
}

/* getdent(fd, struct dirent *): 1 per entry, 0 after the last. */
static int64_t sys_getdent(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  struct dirent de;
  int r = fd_getdent(thread_current()->fd, (int)arg[0], &de);
  if (r == 1 && copy_to_user((void *)arg[1], &de, sizeof de) != 0)
    return -EFAULT;
  return r;
}

/* brk(addr): the heap's new end; the old one if it can't move there. */
static int64_t sys_brk(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  return (int64_t)proc_brk(arg[0]);
}

static int64_t sys_read(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  struct file *file = fd_get(thread_current()->fd, (int)arg[0]);
  if (file == NULL)
    return -EBADF;
  return file_read(file, (void *)arg[1], arg[2]);
}

static int64_t sys_write(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  struct file *file = fd_get(thread_current()->fd, (int)arg[0]);
  if (file == NULL)
    return -EBADF;
  return file_write(file, (const void *)arg[1], arg[2]);
}

static int64_t sys_close(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  return fd_close(thread_current()->fd, (int)arg[0]);
}

static int64_t sys_pipe(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  struct file **table = thread_current()->fd;
  int fds[2];
  int err = fd_pipe(table, fds);
  if (err != 0)
    return err;
  if (copy_to_user((void *)arg[0], fds, sizeof fds) != 0) {
    fd_close(table, fds[0]);
    fd_close(table, fds[1]);
    return -EFAULT;
  }
  return 0;
}

static int64_t sys_dup2(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  return fd_dup2(thread_current()->fd, (int)arg[0], (int)arg[1]);
}

/* exec(path, argv): argv is a NULL-terminated array of strings, or NULL. */
static int64_t sys_exec(const uint64_t arg[6], struct trap_frame *f) {
  char path[PATH_MAX];
  int err = user_path(path, arg[0], CAP_FS_READ);
  if (err != 0)
    return err;
  int n;

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
    err = proc_exec(f, path, argv, argc);
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

/* kill(pid): end a process, as if by SIGKILL. An agent reaches only its
 * own session: never agentd, nor anything else human. */
static int64_t sys_kill(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  struct thread *t = thread_find((int)arg[0]);
  if (t == NULL)
    return -ESRCH;
  if (t->pml4 == 0)
    return -EPERM; /* a kernel thread */
  const struct label *me = &thread_current()->label;
  if (me->origin == ORIGIN_AGENT &&
      (t->label.origin != ORIGIN_AGENT || t->label.session != me->session))
    return -ECAPDENIED;
  thread_kill(t, W_SIGNALED(SIGKILL));
  return 0;
}

/* ps(struct procinfo *buf, size_t max): how many processes were written. */
static int64_t sys_ps(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  size_t max = arg[1];
  if (max > PS_MAX)
    max = PS_MAX;
  if (max == 0)
    return 0;
  struct procinfo *buf = kmalloc(max * sizeof *buf);
  if (buf == NULL)
    return -ENOMEM;
  int n = sched_snapshot(buf, (int)max);
  int64_t r = n;
  if (copy_to_user((void *)arg[0], buf, (size_t)n * sizeof *buf) != 0)
    r = -EFAULT;
  kfree(buf);
  return r;
}

/*
 * mark_agent(session, caps, scope): this process, and every one it
 * starts from now on, becomes an agent of `session` holding `caps`, with
 * writes kept under `scope`. Only narrows, so any process may ask.
 */
static int64_t sys_mark_agent(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  char scope[PATH_MAX]; /* a bound to set, not a path to touch */
  int n = copy_str_from_user(scope, (const char *)arg[2], sizeof scope);
  if (n < 0)
    return n;
  return label_narrow(&thread_current()->label, (uint32_t)arg[0],
                      (uint32_t)arg[1], scope);
}

static const struct syscall table[] = {
    [SYS_exit] = {sys_exit, CAP_ANY, "exit"},
    [SYS_getpid] = {sys_getpid, CAP_ANY, "getpid"},
    [SYS_fork] = {sys_fork, CAP_SPAWN, "fork"},
    [SYS_exec] = {sys_exec, CAP_SPAWN, "exec"},
    [SYS_wait] = {sys_wait, CAP_ANY, "wait"},
    [SYS_kill] = {sys_kill, CAP_KILL, "kill"},
    [SYS_read] = {sys_read, CAP_ANY, "read"},
    [SYS_write] = {sys_write, CAP_ANY, "write"},
    [SYS_close] = {sys_close, CAP_ANY, "close"},
    [SYS_pipe] = {sys_pipe, CAP_ANY, "pipe"},
    [SYS_dup2] = {sys_dup2, CAP_ANY, "dup2"},
    [SYS_brk] = {sys_brk, CAP_ANY, "brk"},
    [SYS_open] = {sys_open, CAP_ANY, "open"}, /* its flags decide */
    [SYS_mkdir] = {sys_mkdir, CAP_FS_WRITE, "mkdir"},
    [SYS_unlink] = {sys_unlink, CAP_FS_WRITE, "unlink"},
    [SYS_rename] = {sys_rename, CAP_FS_WRITE, "rename"},
    [SYS_stat] = {sys_stat, CAP_FS_READ, "stat"},
    [SYS_getdent] = {sys_getdent, CAP_FS_READ, "getdent"},
    [SYS_ps] = {sys_ps, CAP_ANY, "ps"},
    [SYS_mark_agent] = {sys_mark_agent, CAP_ANY, "mark_agent"},
};

#define NSYSCALLS (sizeof table / sizeof table[0])

/* The kind of call, against the caller's label. Paths are checked
 * further in, by user_path, once the handler knows what they are. */
static int cap_check(uint32_t cap) {
  return cap_allows(&thread_current()->label, cap);
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
