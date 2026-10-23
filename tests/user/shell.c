#include "t.h"

/* Run `sh -c line` with its output on a pipe; the output in `out`, and
 * the shell's exit code, or -1 if something went wrong here. */
static int shell(const char *line, char *out, size_t size) {
  int p[2];
  if (pipe(p) != 0)
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    dup2(p[1], STDOUT_FILENO);
    close(p[1]);
    char *argv[] = {(char *)"sh", (char *)"-c", (char *)line, NULL};
    execv("/bin/sh", argv);
    _exit(99);
  }
  close(p[1]);
  size_t got = 0;
  ssize_t n;
  while (got < size - 1 && (n = read(p[0], out + got, size - 1 - got)) > 0)
    got += (size_t)n;
  out[got] = '\0';
  close(p[0]);

  int status;
  if (wait(&status) != pid || !WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

int main(void) {
  char out[256];

  /* A pipeline, the week's goal: /bin holds more than one program. */
  if (shell("ls /bin | wc -l", out, sizeof out) != 0)
    return 1;
  if (atoi(out) < 2)
    return 2;

  /* Words, quoting, and the exit status of the last command. */
  if (shell("echo one 'two  three'", out, sizeof out) != 0)
    return 3;
  if (strcmp(out, "one two  three\n") != 0)
    return 4;
  if (shell("exit 7", out, sizeof out) != 7)
    return 5;
  if (shell("/bin/nope", out, sizeof out) != 127)
    return 6;

  /* Redirection: into a file, appended to, and back out of it. */
  if (shell("echo first > /shtest; echo second >> /shtest", out, sizeof out) !=
      0)
    return 7;
  if (shell("wc -l < /shtest", out, sizeof out) != 0 || atoi(out) != 2)
    return 8;
  if (shell("cat /shtest | wc -c", out, sizeof out) != 0 || atoi(out) != 13)
    return 9;
  if (unlink("/shtest") != 0)
    return 10;

  /* cd is the shell's own: the paths the kernel sees stay absolute. */
  if (shell("mkdir /work", out, sizeof out) != 0)
    return 11;
  if (shell("cd /work && pwd", out, sizeof out) != 0 ||
      strcmp(out, "/work\n") != 0)
    return 12;
  if (shell("cd /work && echo deep > notes.txt && cat notes.txt", out,
            sizeof out) != 0 ||
      strcmp(out, "deep\n") != 0)
    return 13;
  if (shell("cat /work/notes.txt", out, sizeof out) != 0 ||
      strcmp(out, "deep\n") != 0)
    return 14;

  /* '..' is resolved before the kernel is asked, so what it checks is
   * still a path stated in full. */
  if (shell("cd /work && cd .. && pwd", out, sizeof out) != 0 ||
      strcmp(out, "/\n") != 0)
    return 15;
  if (shell("cd /work && cat ../work/notes.txt", out, sizeof out) != 0 ||
      strcmp(out, "deep\n") != 0)
    return 16;
  if (shell("cd /nowhere", out, sizeof out) == 0)
    return 17;
  if (shell("cd /work/notes.txt", out, sizeof out) == 0) /* not a directory */
    return 18;

  /* echo's words are text: the cwd is not applied to them. */
  if (shell("cd /work && echo notes.txt", out, sizeof out) != 0 ||
      strcmp(out, "notes.txt\n") != 0)
    return 19;

  /* A path is rewritten even when it is what the command creates. */
  if (shell("cd /work && mkdir deeper && cd deeper && pwd", out, sizeof out) !=
          0 ||
      strcmp(out, "/work/deeper\n") != 0)
    return 20;
  if (shell("rm /work/deeper", out, sizeof out) != 0)
    return 21;

  /* ls has no way to ask where it is, so the shell tells it. */
  if (shell("cd /work && ls", out, sizeof out) != 0 ||
      strcmp(out, "notes.txt\n") != 0)
    return 22;

  if (shell("rm /work/notes.txt && rm /work", out, sizeof out) != 0)
    return 23;

  return 0;
}
