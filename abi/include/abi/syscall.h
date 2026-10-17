#pragma once

/*
 * System calls: `int 0x80` with the number in rax and up to six arguments
 * in rdi, rsi, rdx, r10, r8, r9. The result comes back in rax: >= 0 on
 * success, -errno on failure. Every other register is preserved.
 */
#define SYSCALL_VECTOR 0x80

#define SYS_exit        0 /* (int code): never returns */
                          /* 1: retired (was debug_write; use write(1)) */
#define SYS_getpid      2 /* (void): this process's pid */
#define SYS_fork        3 /* (void): the child's pid; 0 in the child */
#define SYS_exec        4 /* (const char *path, char *const argv[]): only
                             returns on failure */
#define SYS_wait        5 /* (int *status): a dead child's pid (abi/wait.h) */
#define SYS_kill        6 /* (int pid): ends it with SIGKILL */
#define SYS_read        7 /* (int fd, void *buf, size_t n): bytes, 0 at EOF */
#define SYS_write       8 /* (int fd, const void *buf, size_t n): bytes */
#define SYS_close       9 /* (int fd) */
#define SYS_pipe       10 /* (int fds[2]): fds[0] reads what fds[1] writes */
#define SYS_dup2       11 /* (int oldfd, int newfd): newfd */
#define SYS_brk        12 /* (void *end): the heap's end after the call;
                             unchanged if it can't move (brk(0) asks) */

/* A new program starts with 0, 1 and 2 open on the console; fork shares
 * the parent's descriptors, and exec keeps them. */
