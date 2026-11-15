#include <abi/syscall.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/agent.h>
#include <sys/audit.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int errno;

/* int 0x80: number in rax, arguments in rdi, rsi, rdx; -errno on failure. */
static long syscall3(long nr, long a, long b, long c) {
  long ret;
  __asm__ __volatile__("int $0x80"
                       : "=a"(ret)
                       : "a"(nr), "D"(a), "S"(b), "d"(c)
                       : "memory");
  return ret;
}

/* The same, with a fourth argument in r10. */
static long syscall4(long nr, long a, long b, long c, long d) {
  long ret;
  register long r10 __asm__("r10") = d;
  __asm__ __volatile__("int $0x80"
                       : "=a"(ret)
                       : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10)
                       : "memory");
  return ret;
}

/* The kernel's -errno into errno and -1. */
static long check(long r) {
  if (r < 0 && r > -4096) {
    errno = (int)-r;
    return -1;
  }
  return r;
}

ssize_t read(int fd, void *buf, size_t n) {
  return check(syscall3(SYS_read, fd, (long)buf, (long)n));
}

ssize_t write(int fd, const void *buf, size_t n) {
  return check(syscall3(SYS_write, fd, (long)buf, (long)n));
}

int close(int fd) { return check(syscall3(SYS_close, fd, 0, 0)); }
int pipe(int fds[2]) { return check(syscall3(SYS_pipe, (long)fds, 0, 0)); }

int dup2(int oldfd, int newfd) {
  return check(syscall3(SYS_dup2, oldfd, newfd, 0));
}

pid_t fork(void) { return check(syscall3(SYS_fork, 0, 0, 0)); }
pid_t getpid(void) { return syscall3(SYS_getpid, 0, 0, 0); }

int execv(const char *path, char *const argv[]) {
  return check(syscall3(SYS_exec, (long)path, (long)argv, 0));
}

void _exit(int code) {
  syscall3(SYS_exit, code, 0, 0);
  __builtin_unreachable();
}

pid_t wait(int *status) { return check(syscall3(SYS_wait, (long)status, 0, 0)); }

int kill(pid_t pid, int sig) {
  if (sig != SIGKILL) {
    errno = EINVAL;
    return -1;
  }
  return check(syscall3(SYS_kill, pid, 0, 0));
}

int open(const char *path, int flags) {
  return check(syscall3(SYS_open, (long)path, flags, 0));
}

int mkdir(const char *path) { return check(syscall3(SYS_mkdir, (long)path, 0, 0)); }
int unlink(const char *path) { return check(syscall3(SYS_unlink, (long)path, 0, 0)); }

int rename(const char *from, const char *to) {
  return check(syscall3(SYS_rename, (long)from, (long)to, 0));
}

int stat(const char *path, struct stat *st) {
  return check(syscall3(SYS_stat, (long)path, (long)st, 0));
}

int getdent(int fd, struct dirent *d) {
  return check(syscall3(SYS_getdent, fd, (long)d, 0));
}

/* The kernel's brk never fails outright: it answers with where the end is
 * now, which is the old end if it couldn't move. */
int brk(void *end) {
  if ((void *)syscall3(SYS_brk, (long)end, 0, 0) != end) {
    errno = ENOMEM;
    return -1;
  }
  return 0;
}

void *sbrk(intptr_t inc) {
  char *old = (char *)syscall3(SYS_brk, 0, 0, 0);
  if (inc != 0 && brk(old + inc) != 0)
    return (void *)-1;
  return old;
}

int ps(struct procinfo *buf, size_t max) {
  return check(syscall3(SYS_ps, (long)buf, (long)max, 0));
}

int mark_agent(uint32_t session, uint32_t caps, const char *scope,
               uint32_t max_procs) {
  return check(
      syscall4(SYS_mark_agent, session, caps, (long)scope, max_procs));
}

int kill_session(uint32_t session) {
  return check(syscall3(SYS_kill_session, session, 0, 0));
}

int audit_read(struct audit_record *buf, size_t max, uint64_t after) {
  return check(syscall3(SYS_audit_read, (long)buf, (long)max, (long)after));
}
