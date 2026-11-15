#include "util.h"

#include <fcntl.h>
#include <sys/agent.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * The agent daemon: the guest end of /dev/agent.
 *
 * The host sends one command per line. For each, agentd runs `sh -c LINE`
 * and writes back everything it printed (stdout and stderr together),
 * then one last line, "\0EXIT <code>\n". The NUL keeps that line from
 * being mistaken for output. A command killed by a signal reports
 * 128 plus the signal, as a shell would.
 *
 * Every command runs as an agent: the child marks itself before exec,
 * so sh and all it starts carry the label. agentd itself stays human.
 */

#define LINE_MAX 4096

/* The one profile until profiles come from a config file. */
#define AGENT_SESSION 1
#define AGENT_CAPS    CAP_ALL
#define AGENT_SCOPE   "/home/work"
#define AGENT_MAX_PROCS 32 /* in the session at once: a fork bomb stops here */

/* The first newline in the `n` bytes at `buf`, or NULL (libc has no memchr). */
static char *find_newline(char *buf, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '\n')
      return buf + i;
  return NULL;
}

/* Run `line` under sh -c, its output written to `agent`. */
static void run(int agent, char *line) {
  int fds[2];
  if (pipe(fds) != 0) {
    dprintf(agent, "agentd: pipe failed\n");
    return;
  }

  int pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    dprintf(agent, "agentd: fork failed\n");
    return;
  }
  if (pid == 0) {
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    close(agent);
    if (mark_agent(AGENT_SESSION, AGENT_CAPS, AGENT_SCOPE,
                   AGENT_MAX_PROCS) != 0) {
      complain("agentd", "mark_agent", errno);
      _exit(126); /* never run a command unlabeled */
    }
    char *const argv[] = {"sh", "-c", line, NULL};
    execv("/bin/sh", argv);
    _exit(127);
  }

  close(fds[1]);
  char buf[256];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0)
    write_all(agent, buf, (size_t)n);
  close(fds[0]);

  int status = 0;
  wait(&status);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  dprintf(agent, "%cEXIT %d\n", '\0', code);
}

int main(void) {
  int agent = open("/dev/agent", O_RDWR);
  if (agent < 0) {
    complain("agentd", "/dev/agent", errno);
    return 1;
  }

  /* The scope has to exist for the agent to work in it. */
  if ((mkdir("/home") != 0 && errno != EEXIST) ||
      (mkdir(AGENT_SCOPE) != 0 && errno != EEXIST)) {
    complain("agentd", AGENT_SCOPE, errno);
    return 1;
  }

  char line[LINE_MAX];
  size_t len = 0; /* bytes held in `line`, not yet run */
  for (;;) {
    ssize_t n = read(agent, line + len, sizeof line - len);
    if (n <= 0)
      return 1;
    len += (size_t)n;

    char *nl;
    while ((nl = find_newline(line, len)) != NULL) {
      *nl = '\0';
      run(agent, line);
      size_t used = (size_t)(nl - line) + 1;
      memmove(line, line + used, len - used);
      len -= used;
    }
    if (len == sizeof line) {
      dprintf(agent, "agentd: line too long\n");
      len = 0; /* drop it: the host sees the complaint, not a hang */
    }
  }
}
