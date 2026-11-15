#include "syscall.h"

#include <abi/errno.h>
#include <abi/proc.h>
#include <abi/syscall.h>
#include <abi/wait.h>
#include <audit.h>
#include <cap.h>
#include <file.h>
#include <fs.h>
#include <idt.h>
#include <kstring.h>
#include <kprintf.h>
#include <path.h>
#include <proc.h>
#include <session.h>
#include <sched.h>
#include <slab.h>
#include <stddef.h>
#include <usercopy.h>
#include <vmm.h>

/* ps() reports at most this many processes per call. */
#define PS_MAX 64
/* audit_read() hands back at most this many records per call. */
#define AUDIT_READ_MAX 16

/* `f` is the caller's saved registers, for the calls that replace them
 * (fork copies them, exec rewrites them). Most ignore it. */
typedef int64_t (*syscall_fn)(const uint64_t arg[6], struct trap_frame *f);

/*
 * The capability a syscall needs (cap.h). Every table entry must name
 * one: dispatch() refuses an entry left at CAP_UNDECLARED, so forgetting
 * to think about a new syscall's permission fails closed.
 */
/* For a call whose path decides: the handler passes every path through
 * user_path with what it needs (open: by its flags), which checks the
 * caps as well as the scope, so a refusal is logged with the path. */
#define CAP_BY_PATH CAP_ANY

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

static const char *syscall_name(uint32_t nr);

/* `src` into `dst` (`size` bytes), cut short if it must be. */
static void copy_trunc(char *dst, const char *src, size_t size) {
  size_t n = strlen(src);
  if (n >= size)
    n = size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

/* A record about the calling process, in the syscall it is making. */
static struct audit_record audit_record_of(uint32_t kind, uint32_t need,
                                           int err, const char *path) {
  struct thread *t = thread_current();
  struct audit_record r = {
      .pid = t->tid,
      .session = t->label.session,
      .kind = kind,
      .need = need,
      .err = err,
  };
  copy_trunc(r.syscall, syscall_name(t->syscall), sizeof r.syscall);
  copy_trunc(r.path, path != NULL ? path : "", sizeof r.path);
  return r;
}

void syscall_audit(uint32_t kind, uint32_t need, int err, const char *path) {
  struct audit_record r = audit_record_of(kind, need, err, path);
  audit_log(&r);
}

/* The child gets a copy of everything: memory, registers (with rax 0, so
 * fork returns 0 there), and the parent's name. An agent's child counts
 * against its session's limit. */
static int64_t sys_fork(const uint64_t arg[6], struct trap_frame *f) {
  (void)arg;
  struct thread *parent = thread_current();
  int agent = parent->label.origin == ORIGIN_AGENT;
  if (agent) {
    sched_reap(); /* the session's dead free their places first */
    int err = session_spawn(parent->label.session);
    if (err != 0) {
      syscall_audit(AUDIT_DENIED, CAP_SPAWN, err, NULL);
      return err;
    }
  }
  uint64_t pml4 = vmm_space_new();
  if (pml4 == 0 || vmm_space_copy(pml4, parent->pml4) != 0) {
    if (pml4 != 0)
      vmm_space_free(pml4);
    if (agent)
      session_leave(parent->label.session);
    return -ENOMEM;
  }
  struct trap_frame tf = *f;
  tf.rax = 0;
  /* Interrupts are off (interrupt gate): the child can't run before it has
   * its files, nor die before we read its tid. */
  struct thread *child = thread_create_user(parent->name, pml4, &tf, parent);
  if (child == NULL) {
    if (agent)
      session_leave(parent->label.session);
    return -ENOMEM;
  }
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
  int err = cap_path(&thread_current()->label, out, need);
  if (err == -ECAPDENIED)
    syscall_audit(AUDIT_DENIED, need, err, out);
  return err;
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
  if (err == 0)
    err = fs_mkdir(path);
  struct node *n;
  if (err == 0 && fs_open(path, O_RDONLY, &n) == 0) {
    file_note_creator(n);
    fs_put(n);
  }
  return err;
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
      (t->label.origin != ORIGIN_AGENT || t->label.session != me->session)) {
    syscall_audit(AUDIT_DENIED, CAP_KILL, -ECAPDENIED, NULL);
    return -ECAPDENIED;
  }
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
 * mark_agent(session, caps, scope, max_procs): this process, and every
 * one it starts from now on, becomes an agent of `session` holding
 * `caps`, with writes kept under `scope`, the session holding at most
 * `max_procs` processes. Only narrows, so any process may ask. Both
 * outcomes are logged: a refusal is an attempt to widen.
 */
static int64_t sys_mark_agent(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  char scope[PATH_MAX]; /* a bound to set, not a path to touch */
  int n = copy_str_from_user(scope, (const char *)arg[2], sizeof scope);
  if (n < 0)
    return n;
  struct label *l = &thread_current()->label;
  uint32_t session = (uint32_t)arg[0], max_procs = (uint32_t)arg[3];
  if (max_procs == 0)
    return -EINVAL;
  struct label narrowed = *l; /* both or neither: label and session */
  int err = label_narrow(&narrowed, session, (uint32_t)arg[1], scope);
  if (err == 0 && l->origin == ORIGIN_HUMAN)
    err = session_enter(session, max_procs); /* it counts from now */
  else if (err == 0)
    session_lower(session, max_procs);
  if (err == 0)
    *l = narrowed;
  if (err == 0)
    syscall_audit(AUDIT_MARK, l->caps, 0, l->scope);
  else if (err == -EPERM || err == -EAGAIN)
    syscall_audit(AUDIT_DENIED, (uint32_t)arg[1], err, scope);
  return err;
}

/* kill_session(session): every process in it ends, as if by SIGKILL. */
static int64_t sys_kill_session(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  uint32_t session = (uint32_t)arg[0];
  int n = sched_kill_session(session);
  struct audit_record r = audit_record_of(AUDIT_KILL, 0, 0, NULL);
  r.session = session; /* the one ended, not the caller's (none) */
  audit_log(&r);
  return n;
}

/* audit_read(buf, max, after): the records after seq `after`, how many. */
static int64_t sys_audit_read(const uint64_t arg[6], struct trap_frame *f) {
  (void)f;
  size_t max = arg[1] < AUDIT_READ_MAX ? arg[1] : AUDIT_READ_MAX;
  if (max == 0)
    return 0;
  struct audit_record *buf = kmalloc(max * sizeof *buf);
  if (buf == NULL)
    return -ENOMEM;
  size_t n = audit_read(buf, max, arg[2]);
  int64_t r = (int64_t)n;
  if (copy_to_user((void *)arg[0], buf, n * sizeof *buf) != 0)
    r = -EFAULT;
  kfree(buf);
  return r;
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
    [SYS_open] = {sys_open, CAP_BY_PATH, "open"},
    [SYS_mkdir] = {sys_mkdir, CAP_BY_PATH, "mkdir"},
    [SYS_unlink] = {sys_unlink, CAP_BY_PATH, "unlink"},
    [SYS_rename] = {sys_rename, CAP_BY_PATH, "rename"},
    [SYS_stat] = {sys_stat, CAP_BY_PATH, "stat"},
    [SYS_getdent] = {sys_getdent, CAP_FS_READ, "getdent"},
    [SYS_ps] = {sys_ps, CAP_ANY, "ps"},
    [SYS_mark_agent] = {sys_mark_agent, CAP_ANY, "mark_agent"},
    [SYS_audit_read] = {sys_audit_read, CAP_HUMAN, "audit_read"},
    [SYS_kill_session] = {sys_kill_session, CAP_HUMAN, "kill_session"},
};

#define NSYSCALLS (sizeof table / sizeof table[0])

static const char *syscall_name(uint32_t nr) {
  return nr < NSYSCALLS && table[nr].name != NULL ? table[nr].name : "?";
}

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
  thread_current()->syscall = (uint32_t)nr;
  int err = cap_check(s->cap);
  if (err != 0) {
    syscall_audit(AUDIT_DENIED, s->cap, err, NULL);
    f->rax = err;
    return;
  }

  const uint64_t arg[6] = {f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9};
  f->rax = s->fn(arg, f);
}

void syscall_init(void) { idt_register(SYSCALL_VECTOR, dispatch); }
