#pragma once

/*
 * System calls: `int 0x80` with the number in rax and up to six arguments
 * in rdi, rsi, rdx, r10, r8, r9. The result comes back in rax: >= 0 on
 * success, -errno on failure. Every other register is preserved.
 */
#define SYSCALL_VECTOR 0x80

#define SYS_exit        0 /* (int code): never returns */
#define SYS_debug_write 1 /* (const char *buf, size_t len): to the console;
                             returns len. Temporary, until write(2) has fds */
#define SYS_getpid      2 /* (void): this process's pid */
#define SYS_fork        3 /* (void): the child's pid; 0 in the child */
#define SYS_exec        4 /* (const char *path, char *const argv[]): only
                             returns on failure */
#define SYS_wait        5 /* (int *status): a dead child's pid (abi/wait.h) */
#define SYS_kill        6 /* (int pid): ends it with SIGKILL */
