#pragma once

#include <sys/types.h>

/* System calls. On failure each returns -1 (sbrk: (void *)-1) and sets
 * errno. A program starts with 0, 1, 2 open on the console. */

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
int pipe(int fds[2]);
int dup2(int oldfd, int newfd);
int unlink(const char *path); /* a file, or an empty directory */

pid_t fork(void);
pid_t getpid(void);
/* Only returns on failure. `argv` ends with NULL; NULL means none. */
int execv(const char *path, char *const argv[]);
__attribute__((noreturn)) void _exit(int code);

/* Move the heap's end to `end`; or by `inc`, returning the old end. */
int brk(void *end);
void *sbrk(intptr_t inc);
