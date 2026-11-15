#include "t.h"

#include <sys/agent.h>

/*
 * Agent sessions: a fork bomb stops at the session's process limit, the
 * limit only goes down, and kill_session ends the lot. Only a human may
 * pull that switch.
 */

#define SESSION 9

/* Block until killed: read a pipe nobody ever writes. */
static void hang(int fd) {
  char c;
  for (;;)
    read(fd, &c, 1);
}

/* Fork until refused; how many forks succeeded, the children left
 * hanging on `fd`. -1 if the refusal wasn't EAGAIN. */
static int bomb(int fd) {
  int n = 0;
  for (;;) {
    int pid = fork();
    if (pid == 0)
      hang(fd);
    if (pid < 0)
      return errno == EAGAIN ? n : -1;
    n++;
  }
}

int main(void) {
  int never[2], report[2];
  if (pipe(never) != 0 || pipe(report) != 0)
    return 1;

  /* An agent with room for 4 forks 3, then is refused; it hangs too. */
  int bomber = fork();
  if (bomber == 0) {
    if (mark_agent(SESSION, CAP_SPAWN, "/", 4) != 0)
      _exit(99);
    /* a bigger limit asked for again changes nothing */
    if (mark_agent(SESSION, CAP_SPAWN, "/", 100) != 0)
      _exit(98);
    char c = bomb(never[0]) == 3 ? 'y' : 'n';
    write(report[1], &c, 1);
    hang(never[0]);
  }
  char c = 0;
  if (bomber < 0 || read(report[0], &c, 1) != 1)
    return 2;
  if (c != 'y')
    return 3;

  /* The switch: all four, bomber and children, end at once. */
  if (kill_session(SESSION) != 4)
    return 4;
  int status;
  if (wait(&status) != bomber || !WIFSIGNALED(status) ||
      WTERMSIG(status) != SIGKILL)
    return 5;
  if (kill_session(SESSION) != 0) /* nothing left */
    return 6;

  /* The session went with its last process: marking into it again
   * starts afresh. A child that exits frees its place once reaped. */
  int pid = fork();
  if (pid == 0) {
    if (mark_agent(SESSION, CAP_SPAWN, "/", 2) != 0)
      _exit(10);
    int child = fork();
    if (child == 0)
      _exit(0);
    if (child < 0)
      _exit(11);
    if (!FAILS(fork(), EAGAIN)) /* 2 of 2 */
      _exit(12);
    if (wait(NULL) != child)
      _exit(13);
    child = fork(); /* the zombie is gone: room again */
    if (child == 0)
      _exit(0);
    if (child < 0 || wait(NULL) != child)
      _exit(14);

    /* No agent pulls the switch, not even on its own session. */
    if (!FAILS(kill_session(SESSION), ECAPDENIED))
      _exit(15);
    if (!FAILS(kill_session(SESSION + 1), ECAPDENIED))
      _exit(16);
    _exit(0);
  }
  if (pid < 0 || wait(&status) != pid)
    return 7;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return WIFEXITED(status) ? WEXITSTATUS(status) : 8;

  /* A limit of 0 holds nobody, not even the one asking. */
  if (!FAILS(mark_agent(SESSION, CAP_SPAWN, "/", 0), EINVAL))
    return 20;

  close(never[0]);
  close(never[1]);
  close(report[0]);
  close(report[1]);
  return 0;
}
