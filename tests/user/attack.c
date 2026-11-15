#include "t.h"
#include "../../userland/util.h" /* errname */

#include <sys/agent.h>
#include <sys/audit.h>
#include <sys/proc.h>

/*
 * The rows of the escape suite that a shell line can't express, as one
 * program an agent runs through agentd (tests/qemu/escape_matrix.py).
 * It stands for any code an agent gets to run: under configurations A
 * and B it runs unlabelled, like everything else the agent starts.
 *
 *   attack label PATH       mark itself wider, or into another session,
 *                           then unlink PATH
 *   attack pointers         system calls with addresses not the caller's
 *   attack audit            read the audit log
 *   attack open PATH        open PATH (the agent channel)
 *   attack kill NAME        SIGKILL the process called NAME
 *   attack scatter          three processes outside the tree, hanging
 *   attack bomb             fork until refused, the children hanging
 *   attack plant N PATH     (a human) write PATH as agent session N
 *   attack switch           (a human) kill_session(1): how many ended
 *
 * Exit 0 means refused, the attack blocked; the driver also checks the
 * state it was after.
 */

#define BOMB_CAP 64 /* stop here if nothing else does */

/* Hang until killed, holding nothing agentd waits on: with stdout still
 * open, agentd would never see the command's output end. */
static void hang(void) {
  int fds[2];
  for (int fd = 0; fd < 16; fd++)
    close(fd);
  if (pipe(fds) != 0)
    _exit(1);
  char c;
  for (;;)
    read(fds[0], &c, 1); /* the write end is ours: never EOF */
}

static int label(const char *path) {
  int wider = mark_agent(1, CAP_ALL, "/", 64) == 0 ? 0 : errno;
  int other = mark_agent(77, CAP_ALL, "/", 64) == 0 ? 0 : errno;
  printf("mark_agent wider: %s, other session: %s\n",
         wider == 0 ? "ok" : errname(wider), other == 0 ? "ok" : errname(other));
  if (unlink(path) == 0) {
    printf("unlink %s: done\n", path);
    return 1;
  }
  printf("unlink %s: %s\n", path, errname(errno));
  return 0;
}

static int pointers(void) {
  static char *const bad[] = {(char *)8, (char *)0xffffffff80000000UL,
                              (char *)0x0000800000000000UL};
  int refused = 0, tried = 0;
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char *p = bad[i];
    int fds[2];
    if (pipe(fds) != 0 || write(fds[1], "x", 1) != 1)
      return 2;
    const int ok[] = {
        FAILS(open(p, O_RDONLY), EFAULT),
        FAILS(unlink(p), EFAULT),
        FAILS(stat("/", (struct stat *)p), EFAULT),
        FAILS(read(fds[0], p, 8), EFAULT),
        FAILS(write(fds[1], p, 8), EFAULT),
        FAILS(execv(p, NULL), EFAULT),
        FAILS(ps((struct procinfo *)p, 4), EFAULT),
    };
    close(fds[0]);
    close(fds[1]);
    for (size_t k = 0; k < sizeof ok / sizeof ok[0]; k++, tried++)
      refused += ok[k];
  }
  printf("pointers: %d of %d refused with EFAULT\n", refused, tried);
  return refused == tried ? 0 : 1;
}

static int audit(void) {
  struct audit_record r[4];
  int n = audit_read(r, 4, 0);
  if (n >= 0) {
    printf("audit_read: %d records\n", n);
    return 1;
  }
  printf("audit_read: %s\n", errname(errno));
  return 0;
}

static int open_it(const char *path) {
  int fd = open(path, O_WRONLY);
  if (fd >= 0) {
    close(fd);
    printf("open %s: done\n", path);
    return 1;
  }
  printf("open %s: %s\n", path, errname(errno));
  return 0;
}

static int kill_named(const char *name) {
  struct procinfo p[64];
  int n = ps(p, 64);
  for (int i = 0; i < n; i++)
    if (strcmp(p[i].name, name) == 0) {
      if (kill(p[i].pid, SIGKILL) == 0) {
        printf("kill %s (%d): done\n", name, p[i].pid);
        return 1;
      }
      printf("kill %s (%d): %s\n", name, p[i].pid, errname(errno));
      return 0;
    }
  printf("kill: no %s\n", name);
  return 2;
}

static int scatter(void) {
  int mid = fork();
  if (mid == 0) {
    for (int i = 0; i < 3; i++)
      if (fork() == 0)
        hang(); /* orphaned when mid exits: in no tree at all */
    _exit(0);
  }
  int status;
  wait(&status);
  printf("scattered 3\n");
  return 0;
}

static int bomb(void) {
  int n = 0;
  int pid;
  while (n < BOMB_CAP && (pid = fork()) >= 0) {
    if (pid == 0)
      hang();
    n++;
  }
  if (n == BOMB_CAP) {
    printf("bomb: %d forks, never refused\n", n);
    return 1;
  }
  printf("bomb: refused after %d forks: %s\n", n, errname(errno));
  return errno == EAGAIN ? 0 : 1;
}

static int plant(const char *session, const char *path) {
  if (mark_agent((uint32_t)atoi(session), CAP_ALL, "/home/work", 8) != 0) {
    printf("plant: mark_agent: %s\n", errname(errno));
    return 1;
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0 || write(fd, "keep\n", 5) != 5 || close(fd) != 0) {
    printf("plant %s: %s\n", path, errname(errno));
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "";
  if (strcmp(what, "label") == 0 && argc == 3)
    return label(argv[2]);
  if (strcmp(what, "pointers") == 0)
    return pointers();
  if (strcmp(what, "audit") == 0)
    return audit();
  if (strcmp(what, "open") == 0 && argc == 3)
    return open_it(argv[2]);
  if (strcmp(what, "kill") == 0 && argc == 3)
    return kill_named(argv[2]);
  if (strcmp(what, "scatter") == 0)
    return scatter();
  if (strcmp(what, "bomb") == 0)
    return bomb();
  if (strcmp(what, "plant") == 0 && argc == 4)
    return plant(argv[2], argv[3]);
  if (strcmp(what, "switch") == 0) {
    printf("kill_session: %d ended\n", kill_session(1));
    return 0;
  }
  printf("usage: attack label|pointers|audit|open|kill|scatter|bomb|plant|switch\n");
  return 2;
}
