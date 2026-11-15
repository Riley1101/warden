#include "t.h"

#include <sys/agent.h>
#include <sys/audit.h>

/*
 * File labels: what an agent writes runs as that agent, whoever runs it.
 * An agent leaves a "#!" script and a program behind; a human running
 * them gets the agent's limits, through a rename and an overwrite, and
 * an agent of another session can't run or write them at all.
 *
 * `flabel probe` is the program left behind: it exits 0 if it finds
 * itself an agent, 1 if human.
 */

#define SESSION 11
#define OTHER   12

static int probe(void) {
  struct audit_record r;
  int agent = FAILS(audit_read(&r, 1, 0), ECAPDENIED);
  if (agent && !FAILS(unlink("/home/victim"), ECAPDENIED))
    return 2; /* an agent, but not held to its scope */
  return agent ? 0 : 1;
}

/* `src` copied over `dst`, created or truncated. */
static int copy(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
  if (in < 0 || out < 0)
    return 0;
  char buf[512];
  ssize_t n;
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, (size_t)n) != n)
      return 0;
  return n == 0 && close(in) == 0 && close(out) == 0;
}

static int put(const char *path, const char *s) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  return fd >= 0 && write(fd, s, strlen(s)) == (ssize_t)strlen(s) &&
         close(fd) == 0;
}

/* Created, and never written. */
static int touch_empty(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT);
  return fd >= 0 && close(fd) == 0;
}

static int exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* Run `path` (argv[1] `arg`, or none) in a child: its exit code, 100 +
 * errno if exec failed, 99 if it died otherwise. */
static int run(const char *path, const char *arg) {
  int pid = fork();
  if (pid == 0) {
    char *const argv[] = {"x", (char *)arg, NULL};
    execv(path, argv);
    _exit(100 + errno);
  }
  int status;
  if (pid < 0 || wait(&status) != pid || !WIFEXITED(status))
    return 99;
  return WEXITSTATUS(status);
}

/* Run `fn` in a child marked as an agent; its exit code, 99 if it
 * couldn't mark. */
static int as_agent(uint32_t session, uint32_t caps, const char *scope,
                    int (*fn)(void)) {
  int pid = fork();
  if (pid == 0) {
    if (mark_agent(session, caps, scope, 8) != 0)
      _exit(99);
    _exit(fn());
  }
  int status;
  if (pid < 0 || wait(&status) != pid || !WIFEXITED(status))
    return 98;
  return WEXITSTATUS(status);
}

/* The agent that leaves things behind, in its own scope. */
static int leave_behind(void) {
  if (mkdir("/home/work/d") != 0 || !touch_empty("/home/work/e"))
    return 13;
  if (!put("/home/work/s", "#!/bin/sh\nrm /home/victim\n"))
    return 10;
  if (!copy("/test/flabel", "/home/work/p"))
    return 11;
  return 0;
}

/* Another session's agent, with the run of the whole tree. */
static int intruder(void) {
  char *const argv[] = {"x", "probe", NULL};
  if (!FAILS(execv("/home/p2", argv), ECAPDENIED))
    return 20;
  int fd = open("/home/p2", O_WRONLY);
  if (fd < 0)
    return 21; /* in scope: opening is fine */
  if (!FAILS(write(fd, "x", 1), ECAPDENIED))
    return 22; /* but it is session 11's file */
  close(fd);
  return 0;
}

/* The writer's own session, narrower: it may run its session's file. */
static int sibling(void) {
  return run("/home/p2", "probe") == 0 ? 0 : 30;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "probe") == 0)
    return probe();

  if ((mkdir("/home") != 0 && errno != EEXIST) || mkdir("/home/work") != 0 ||
      !put("/home/victim", "") || !put("/home/victim2", ""))
    return 1;

  int r = as_agent(SESSION, CAP_ALL, "/home/work", leave_behind);
  if (r != 0)
    return r;
  struct stat st;
  if (stat("/home/work/s", &st) != 0 || st.agent != 1 ||
      st.session != SESSION || st.caps != CAP_ALL)
    return 9;
  if (stat("/home/victim", &st) != 0 || st.agent != 0 || st.session != 0)
    return 12;
  /* what it made without writing is its too */
  if (stat("/home/work/d", &st) != 0 || st.agent != 1 ||
      stat("/home/work/e", &st) != 0 || st.agent != 1)
    return 14;

  /* The controls: a human's script and program run as human. */
  if (!put("/home/hs", "#!/bin/sh\nrm /home/victim2\n"))
    return 2;
  if (run("/home/hs", NULL) != 0 || exists("/home/victim2"))
    return 3;
  if (run("/test/flabel", "probe") != 1)
    return 4;

  /* The agent's script, run by a human: its rm is refused. */
  if (run("/home/work/s", NULL) == 0 || !exists("/home/victim"))
    return 5;
  /* The agent's program, run by a human, finds itself an agent. */
  if ((r = run("/home/work/p", "probe")) != 0)
    return 40 + r;

  /* Moving it, and writing over it as a human, don't wash it. */
  if (rename("/home/work/p", "/home/p2") != 0)
    return 6;
  if ((r = run("/home/p2", "probe")) != 0)
    return 50 + r;
  if (!copy("/test/flabel", "/home/p2"))
    return 7;
  if ((r = run("/home/p2", "probe")) != 0)
    return 60 + r;

  if ((r = as_agent(OTHER, CAP_ALL, "/", intruder)) != 0)
    return r;
  if ((r = as_agent(SESSION, CAP_FS_READ | CAP_SPAWN, "/home/work",
                    sibling)) != 0)
    return r;

  /* Unlinking is what clears it: the tree goes, nothing left behind. */
  if (unlink("/home/work/d") != 0 || unlink("/home/work/e") != 0 ||
      unlink("/home/work/s") != 0 || unlink("/home/p2") != 0 ||
      unlink("/home/hs") != 0 || unlink("/home/victim") != 0 ||
      unlink("/home/work") != 0 || unlink("/home") != 0)
    return 8;
  return 0;
}
