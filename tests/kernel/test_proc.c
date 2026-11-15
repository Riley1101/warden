#ifdef WARDEN_TEST
#include "ktest.h"
#include "agent.h"
#include "cpu.h"
#include "file.h"
#include "kprintf.h"
#include "pmm.h"
#include "proc.h"
#include "sched.h"
#include "slab.h"

#include <abi/errno.h>
#include <abi/wait.h>

/*
 * Real programs from the initramfs (tests/user, in /test). Each runs
 * as a child of this (the boot) thread and reports through its exit
 * status: 0 means every check inside it passed.
 */

static size_t free_frames(void) {
    sched_reap();
    kmalloc_shrink();
    return pmm_free_count();
}

/* Run `path` to completion; its wait status, or -1 if it didn't start. */
static int run(const char *path, const char *const *argv, int argc) {
    int pid = proc_spawn(path, argv, argc, thread_current());
    if (pid < 0)
        return -1;
    int status;
    if (thread_wait(&status) != pid)
        return -1;
    return status;
}

/* Run it, and check it left nothing behind. */
#define RUN_EXPECT(want, path, ...)                                            \
    do {                                                                       \
        const char *argv_[] = {__VA_ARGS__};                                   \
        size_t before_ = free_frames();                                        \
        int st_ = run(path, argv_, sizeof argv_ / sizeof argv_[0]);            \
        if (st_ != (want))                                                     \
            kprintf("    %s: status %x, want %x\n", path, st_, (want));        \
        KASSERT(st_ == (want));                                                \
        KASSERT(free_frames() == before_);                                     \
    } while (0)

int test_proc_hello(void) {
    RUN_EXPECT(W_EXITCODE(7), "/test/hello", "hello");
    KASSERT(thread_wait(&(int){0}) == -ECHILD); /* collected: none left */
    return 0;
}

int test_proc_args(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/args", "args", "a", "bc");
    RUN_EXPECT(W_EXITCODE(1), "/test/args", "args"); /* argc checked */
    return 0;
}

int test_proc_fork(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/fork", "fork");
    return 0;
}

int test_proc_kill(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/kill", "kill");
    return 0;
}

/* exec's last step replaces the program with /test/hello: hence 7. */
int test_proc_exec(void) {
    RUN_EXPECT(W_EXITCODE(7), "/test/exec", "exec");
    return 0;
}

int test_proc_pipe(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/pipe", "pipe");
    return 0;
}

int test_proc_dup2(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/dup2", "dup2");
    return 0;
}

/* Type `s` at the console, as the serial poll would. */
static void type(const char *s) {
    uint64_t flags = irq_save();
    for (; *s != '\0'; s++)
        console_feed(*s);
    irq_restore(flags);
}

/* Programs start with 0, 1, 2 on the console: echo's line shows up, and
 * what is typed reaches a reader a line at a time, with backspace rubbing
 * a character out and Ctrl-D ending the input. */
int test_proc_console(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/echo", "echo", "console", "ok");

    type("hex\bllo\n"); /* "hello\n": six bytes */
    type("\x04");        /* nothing typed: end of input */
    RUN_EXPECT(W_EXITCODE(6), "/test/count", "count");
    return 0;
}

int test_proc_libc(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/libc", "libc");
    return 0;
}

/* The agent channel: bytes fed in here, read through /dev/agent there. */
int test_agent_channel(void) {
    uint64_t flags = irq_save();
    for (const char *p = "ping\ntwo"; *p != '\0'; p++)
        agent_feed(*p);
    irq_restore(flags);
    RUN_EXPECT(W_EXITCODE(0), "/test/agent", "agent");
    return 0;
}

/* Labels narrow in the child, and only there: the parent stays human. */
int test_mark_agent(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/mark", "mark");
    KASSERT(thread_current()->label.origin == ORIGIN_HUMAN);
    return 0;
}

/* Denials logged for a human to read, refused to an agent. */
int test_audit(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/audit", "audit");
    return 0;
}

/* The shell in /bin, running pipelines and redirections (tests/user). */
int test_shell(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/shell", "shell");
    return 0;
}

int test_proc_fs(void) {
    RUN_EXPECT(W_EXITCODE(0), "/test/fs", "fs");
    return 0;
}

int test_proc_spawn_errors(void) {
    struct thread *me = thread_current();
    size_t before = free_frames();
    const char *one[] = {"x"};
    KASSERT(proc_spawn("/test/nope", one, 1, me) == -ENOENT);
    KASSERT(proc_spawn("/test/notelf", one, 1, me) == -ENOEXEC);
    KASSERT(proc_spawn("/test", one, 1, me) == -EACCES); /* a directory */

    /* arguments that don't fit the stack's top page */
    static char big[3000];
    for (size_t i = 0; i < sizeof big - 1; i++)
        big[i] = 'x';
    const char *two_big[] = {big, big};
    KASSERT(proc_spawn("/test/hello", two_big, 2, me) == -E2BIG);
    KASSERT(proc_spawn("/test/hello", one, PROC_MAX_ARGS + 1, me) == -E2BIG);

    KASSERT(free_frames() == before);
    return 0;
}

#endif /* ifdef WARDEN_TEST */
