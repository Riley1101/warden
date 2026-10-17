#pragma once

/*
 * Raw system calls: each returns what the kernel did, -errno on failure.
 * The libc wrappers (errno, and the rest) come later and build on these.
 */

#include <abi/errno.h>
#include <abi/syscall.h>
#include <abi/wait.h>
#include <stddef.h>

static inline long syscall3(long nr, long a, long b, long c) {
  long ret;
  __asm__ __volatile__("int $0x80"
                       : "=a"(ret)
                       : "a"(nr), "D"(a), "S"(b), "d"(c)
                       : "memory");
  return ret;
}

static inline __attribute__((noreturn)) void sys_exit(int code) {
  syscall3(SYS_exit, code, 0, 0);
  __builtin_unreachable();
}

static inline long sys_debug_write(const char *buf, size_t len) {
  return syscall3(SYS_debug_write, (long)buf, (long)len, 0);
}

static inline long sys_getpid(void) { return syscall3(SYS_getpid, 0, 0, 0); }
static inline long sys_fork(void) { return syscall3(SYS_fork, 0, 0, 0); }

static inline long sys_exec(const char *path, char *const argv[]) {
  return syscall3(SYS_exec, (long)path, (long)argv, 0);
}

static inline long sys_wait(int *status) {
  return syscall3(SYS_wait, (long)status, 0, 0);
}

static inline long sys_kill(long pid) { return syscall3(SYS_kill, pid, 0, 0); }
