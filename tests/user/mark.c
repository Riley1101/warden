#include "t.h"

#include <sys/agent.h>

/*
 * mark_agent and what it enforces: the label narrows and never widens,
 * it survives fork and exec, and the kernel holds an agent to it.
 *
 * Each enforcement scenario runs in a child that marks itself, so this
 * process stays human and can set up the next. The files an agent must
 * not touch are throwaways made here: if enforcement is missing, the
 * damage stays in /home, and the tests after this one still have /bin.
 */

#define RW (CAP_FS_READ | CAP_FS_WRITE)

#define DENIED(call) FAILS(call, ECAPDENIED)

/* Run `fn` in a child marked with `caps` and `scope`; its exit code, or
 * 99 if it couldn't start or mark. */
static int as_agent(uint32_t caps, const char *scope, int (*fn)(void)) {
  int pid = fork();
  if (pid == 0) {
    if (mark_agent(1, caps, scope) != 0)
      _exit(99);
    _exit(fn());
  }
  int status;
  if (pid < 0 || wait(&status) != pid)
    return 99;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 98;
}

/* A file at `path`, made by whoever calls this. */
static int touch(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT);
  return fd >= 0 && close(fd) == 0;
}

/* A child that blocks until killed, or until `fds` is closed. */
static int sleeper(int fds[2]) {
  if (pipe(fds) != 0)
    return -1;
  int pid = fork();
  if (pid == 0) {
    close(fds[1]);
    char c;
    read(fds[0], &c, 1);
    _exit(0);
  }
  close(fds[0]);
  return pid;
}

static int human_pid; /* a human process, for the agent to aim at */

/* CAP_ALL within /home/work: anything inside, no writes outside. */
static int scoped_writer(void) {
  /* inside the scope, writing works */
  if (mkdir("/home/work/d") != 0 || !touch("/home/work/d/f"))
    return 10;
  if (rename("/home/work/d/f", "/home/work/g") != 0)
    return 11;
  if (unlink("/home/work/g") != 0 || unlink("/home/work/d") != 0)
    return 12;

  /* outside it, by name, by prefix, and by .. */
  if (!DENIED(unlink("/home/victim")))
    return 13;
  if (!DENIED(mkdir("/home/new")))
    return 14;
  if (!DENIED(open("/home/workevil/f", O_WRONLY | O_CREAT)))
    return 15;
  if (!DENIED(open("/home/work/../victim", O_WRONLY)))
    return 16;
  if (!DENIED(open("/home/victim", O_RDONLY | O_TRUNC)))
    return 17;
  /* rename checks both ends: nothing comes in to be deleted, or goes out */
  if (!DENIED(rename("/home/victim", "/home/work/x")))
    return 18;
  if (!DENIED(rename("/home/work/f", "/home/victim2")))
    return 19;

  /* reading isn't scoped */
  int fd = open("/home/victim", O_RDONLY);
  if (fd < 0 || close(fd) != 0)
    return 20;
  struct stat st;
  if (stat("/bin/ls", &st) != 0)
    return 21;

  /* the channel to the host is never an agent's */
  if (!DENIED(open("/dev/agent", O_RDWR)))
    return 22;

  /* kill reaches its own session, and no further */
  if (!DENIED(kill(human_pid, SIGKILL)))
    return 23;
  int fds[2];
  int pid = sleeper(fds);
  if (pid < 0 || kill(pid, SIGKILL) != 0 || wait(NULL) != pid)
    return 24;
  close(fds[1]);
  return 0;
}

/* CAP_FS_READ only: it can look, and that's all. */
static int reader(void) {
  struct stat st;
  if (stat("/home/work/f", &st) != 0)
    return 30;
  int fd = open("/home/work/f", O_RDONLY);
  if (fd < 0 || close(fd) != 0)
    return 31;
  /* in scope, but no CAP_FS_WRITE */
  if (!DENIED(open("/home/work/f", O_WRONLY)))
    return 32;
  if (!DENIED(open("/home/work/new", O_WRONLY | O_CREAT)))
    return 33;
  if (!DENIED(mkdir("/home/work/d")))
    return 34;
  if (!DENIED(unlink("/home/work/f")))
    return 35;
  /* no CAP_SPAWN, no CAP_KILL */
  if (!DENIED(fork()))
    return 36;
  char *const args[] = {"hello", NULL};
  if (!DENIED(execv("/test/hello", args)))
    return 37;
  if (!DENIED(kill(human_pid, SIGKILL)))
    return 38;
  /* asking for the caps back changes nothing */
  if (mark_agent(1, CAP_ALL, "/home/work") != 0 || !DENIED(fork()))
    return 39;
  return 0;
}

/* No caps at all: not even a look. */
static int nothing(void) {
  struct stat st;
  if (!DENIED(stat("/", &st)))
    return 40;
  if (!DENIED(open("/home/work/f", O_RDONLY)))
    return 41;
  /* what every process may do still works */
  if (getpid() <= 0)
    return 42;
  return 0;
}

/* What the label allows, probed: the same again is fine, more is not.
 * It keeps CAP_SPAWN, which the fork and exec after it need. */
static int still_narrow(void) {
  if (mark_agent(1, RW | CAP_SPAWN, "/home/work") != 0)
    return 0;
  if (!FAILS(mark_agent(1, RW, "/home"), EPERM))
    return 0;
  if (!FAILS(mark_agent(2, RW, "/home/work"), EPERM))
    return 0;
  return 1;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "exec") == 0)
    return still_narrow() ? 0 : 50; /* after exec, from below */

  /* The tree the agents work in, made while still human. */
  if ((mkdir("/home") != 0 && errno != EEXIST) || mkdir("/home/work") != 0 ||
      mkdir("/home/workevil") != 0 || !touch("/home/work/f") ||
      !touch("/home/victim"))
    return 1;

  int fds[2];
  human_pid = sleeper(fds);
  if (human_pid < 0)
    return 2;
  int r = as_agent(CAP_ALL, "/home/work", scoped_writer);
  if (r == 0)
    r = as_agent(CAP_FS_READ, "/home/work", reader);
  if (r == 0)
    r = as_agent(0, "/home/work", nothing);
  /* the human it aimed at is still there to end */
  close(fds[1]);
  if (wait(NULL) != human_pid && r == 0)
    r = 3;
  if (r != 0)
    return r;
  /* and the tree goes, so the run leaves nothing behind */
  if (unlink("/home/work/f") != 0 || unlink("/home/victim") != 0 ||
      unlink("/home/workevil") != 0 || unlink("/home/work") != 0 ||
      unlink("/home") != 0)
    return 64;

  /* A bad scope is refused, and leaves the process human. */
  if (!FAILS(mark_agent(1, RW, "relative"), EINVAL))
    return 4;
  if (!FAILS(mark_agent(1, RW, (const char *)8), EFAULT))
    return 5;

  if (mark_agent(1, RW | CAP_SPAWN, "/home/work/") != 0)
    return 6;
  if (!still_narrow())
    return 7;
  /* "/home/workevil" shares the prefix but isn't inside */
  if (!FAILS(mark_agent(1, RW, "/home/workevil"), EPERM))
    return 8;
  if (!FAILS(mark_agent(1, RW, "/home/work/.."), EPERM))
    return 9;

  /* fork copies it */
  int pid = fork();
  if (pid == 0)
    _exit(still_narrow() ? 0 : 1);
  int status;
  if (pid < 0 || wait(&status) != pid || status != 0)
    return 60;

  /* exec keeps it */
  pid = fork();
  if (pid == 0) {
    char *const args[] = {"mark", "exec", NULL};
    execv("/test/mark", args);
    _exit(70);
  }
  if (pid < 0 || wait(&status) != pid || WEXITSTATUS(status) != 0)
    return 61;

  /* narrowing the scope further sticks */
  if (mark_agent(1, CAP_FS_READ, "/home/work/sub") != 0)
    return 62;
  if (!FAILS(mark_agent(1, RW, "/home/work"), EPERM))
    return 63;
  return 0;
}
