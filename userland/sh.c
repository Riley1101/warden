#include "util.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * A small shell.
 *
 *   sh                 read commands from stdin, with a prompt
 *   sh -c "LINE"       run one line
 *   sh FILE            run the lines of FILE
 *
 * A line is one or more pipelines, separated by ';' or '&&' (which runs
 * the rest only if what came before succeeded). A pipeline is commands
 * separated by '|', each with optional '<', '>' and '>>' redirections.
 * Words may be quoted with '' or "";
 * there is no expansion of any kind, and '#' starts a comment. A command
 * without a '/' in it is looked for in /bin.
 *
 * The kernel has no working directory: every path it is given is
 * absolute. `cd` is this shell's own fiction, kept here and applied to
 * the words of a command before it runs (see absolutize). So the kernel
 * still checks a path that was stated in full, and a shell that got the
 * rewriting wrong could not reach anything the kernel would refuse.
 */

#define MAX_TOKENS 128
#define MAX_ARGS   32 /* PROC_MAX_ARGS, the kernel's limit */
#define MAX_CMDS   8
#define LINE_MAX   1024
#define PATH_LIMIT 256 /* as long a path as a command may name */

enum tok_type { T_WORD, T_PIPE, T_LT, T_GT, T_GTGT, T_SEMI, T_AND };

struct tok {
  enum tok_type type;
  char *text; /* T_WORD only */
};

struct cmd {
  char *argv[MAX_ARGS + 1]; /* NULL-terminated, for execv */
  int argc;
  const char *in;  /* '<' file, or NULL */
  const char *out; /* '>' or '>>' file, or NULL */
  int append;
};

static int last_status;

/* ---- the shell's working directory ---------------------------------- */

static char cwd[PATH_LIMIT] = "/";

/*
 * `path` made absolute and tidied into `out`: a relative path hangs off
 * the cwd, and '.' and '..' are resolved by text, which is all they can
 * mean with no links in the filesystem. 0, or -1 if it doesn't fit.
 */
static int resolve(const char *path, char *out, size_t size) {
  char joined[2 * PATH_LIMIT];
  if (path[0] == '/') {
    if (strlcpy(joined, path, sizeof joined) >= sizeof joined)
      return -1;
  } else if ((size_t)snprintf(joined, sizeof joined, "%s/%s", cwd, path) >=
             sizeof joined)
    return -1;

  size_t len = 0;
  out[0] = '\0';
  for (char *p = joined; *p != '\0';) {
    while (*p == '/')
      p++;
    const char *comp = p;
    while (*p != '\0' && *p != '/')
      p++;
    size_t n = (size_t)(p - comp);
    if (n == 0 || (n == 1 && comp[0] == '.'))
      continue;
    if (n == 2 && comp[0] == '.' && comp[1] == '.') {
      char *slash = strrchr(out, '/');
      if (slash != NULL) {
        *slash = '\0';
        len = (size_t)(slash - out);
      }
      continue;
    }
    if (len + n + 2 > size)
      return -1;
    out[len++] = '/';
    memcpy(out + len, comp, n);
    len += n;
    out[len] = '\0';
  }
  if (len == 0)
    strlcpy(out, "/", size);
  return 0;
}

/* ---- parsing -------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/*
 * Cut `line` into tokens, with the words copied into `pool` (LINE_MAX +
 * MAX_TOKENS bytes: the line, plus a terminator per word). Quotes group
 * what is inside them and are dropped; there is no escaping. Returns how
 * many tokens, or -1 for an unterminated quote or too many of them.
 */
static int lex(const char *line, struct tok *tok, int max, char *pool) {
  int n = 0;
  const char *p = line;
  char *w = pool;
  while (*p != '\0') {
    while (is_space(*p))
      p++;
    if (*p == '\0' || *p == '#')
      break;
    if (n == max)
      return -1;

    if (*p == ';' || *p == '&' || *p == '|' || *p == '<' || *p == '>') {
      char op = *p++;
      enum tok_type type;
      if (op == ';')
        type = T_SEMI;
      else if (op == '&') {
        if (*p != '&')
          return -1; /* nothing runs in the background */
        p++;
        type = T_AND;
      } else if (op == '|')
        type = T_PIPE;
      else if (op == '<')
        type = T_LT;
      else {
        type = T_GT;
        if (*p == '>') {
          p++;
          type = T_GTGT;
        }
      }
      tok[n++] = (struct tok){.type = type, .text = NULL};
      continue;
    }

    char *word = w;
    while (*p != '\0' && !is_space(*p) && *p != '|' && *p != '<' &&
           *p != '>' && *p != ';' && *p != '&') {
      if (*p == '\'' || *p == '"') {
        char quote = *p++;
        while (*p != quote) {
          if (*p == '\0')
            return -1; /* the quote is never closed */
          *w++ = *p++;
        }
        p++;
      } else
        *w++ = *p++;
    }
    *w++ = '\0';
    tok[n++] = (struct tok){.type = T_WORD, .text = word};
  }
  return n;
}

/* Tokens into a pipeline. Returns how many commands, or -1 (with a
 * message) if the line doesn't parse. */
static int parse(struct tok *tok, int ntok, struct cmd *cmd, int max) {
  int n = 0;
  cmd[0] = (struct cmd){0};
  for (int i = 0; i < ntok; i++) {
    struct cmd *c = &cmd[n];
    switch (tok[i].type) {
    case T_WORD:
      if (c->argc == MAX_ARGS) {
        dprintf(2, "sh: too many arguments\n");
        return -1;
      }
      c->argv[c->argc++] = tok[i].text;
      break;
    case T_PIPE:
      if (c->argc == 0 || i + 1 == ntok) {
        dprintf(2, "sh: syntax error near '|'\n");
        return -1;
      }
      if (n + 1 == max) {
        dprintf(2, "sh: pipeline too long\n");
        return -1;
      }
      cmd[++n] = (struct cmd){0};
      break;
    default: /* a redirection: the next token names the file */
      if (i + 1 == ntok || tok[i + 1].type != T_WORD) {
        dprintf(2, "sh: syntax error: redirection needs a file\n");
        return -1;
      }
      if (tok[i].type == T_LT)
        c->in = tok[i + 1].text;
      else {
        c->out = tok[i + 1].text;
        c->append = tok[i].type == T_GTGT;
      }
      i++;
      break;
    }
  }
  if (cmd[n].argc == 0)
    return n; /* an empty line, or a trailing comment */
  cmd[n].argv[cmd[n].argc] = NULL;
  return n + 1;
}

/* ---- running -------------------------------------------------------- */

/*
 * Which of a command's words name files. The shell cannot know what a
 * program does with its arguments, so for the programs in /bin it is
 * told; "mkdir work" has to be rewritten although `work` is what is
 * about to be created, and "echo work" must not be.
 */
enum words { WORDS_PATHS, WORDS_TEXT, WORDS_GUESS };

static enum words words_of(const char *cmd) {
  static const char *paths[] = {"ls", "cat", "rm", "mkdir", "mv", "wc"};
  static const char *text[] = {"echo", "sh", "ps"};
  for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
    if (strcmp(cmd, paths[i]) == 0)
      return WORDS_PATHS;
  for (size_t i = 0; i < sizeof text / sizeof text[0]; i++)
    if (strcmp(cmd, text[i]) == 0)
      return WORDS_TEXT;
  return WORDS_GUESS;
}

/*
 * For a command the shell has never heard of: a word with a '/' in it is
 * a path, and a bare word is one if something of that name is there.
 * A guess, and the reason the table above exists.
 */
static int looks_like_path(const char *w) {
  if (strchr(w, '/') != NULL)
    return 1;
  char abs[PATH_LIMIT];
  struct stat st;
  return resolve(w, abs, sizeof abs) == 0 && stat(abs, &st) == 0;
}

/* Storage for the rewritten words. The child is about to exec: it has
 * the whole address space to itself. */
static char argbuf[MAX_ARGS][PATH_LIMIT];

/* The last component of a path: "/bin/ls" is "ls". */
static const char *basename_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

/*
 * A command's relative paths, made absolute, so what reaches the kernel
 * is stated in full. A word starting with '-' is a flag, and `ls` with
 * no path of its own is given the cwd: it has no way to ask for it.
 */
static void absolutize(struct cmd *c) {
  const char *name = basename_of(c->argv[0]);
  enum words words = words_of(name);
  if (words == WORDS_TEXT)
    return;

  int paths = 0;
  for (int i = 1; i < c->argc; i++) {
    char *w = c->argv[i];
    if (w[0] == '-' && w[1] != '\0')
      continue;
    if (words == WORDS_GUESS && !looks_like_path(w))
      continue;
    paths++;
    if (w[0] == '/')
      continue;
    if (resolve(w, argbuf[i], PATH_LIMIT) == 0)
      c->argv[i] = argbuf[i];
  }

  if (paths == 0 && strcmp(name, "ls") == 0 && c->argc < MAX_ARGS) {
    c->argv[c->argc++] = cwd;
    c->argv[c->argc] = NULL;
  }
}

/* A redirection's file, opened at `want`. */
static void redirect(const char *path, int flags, int want) {
  char abs[PATH_LIMIT];
  if (resolve(path, abs, sizeof abs) != 0) {
    complain("sh", path, ENAMETOOLONG);
    _exit(1);
  }
  int fd = open(abs, flags);
  if (fd < 0) {
    complain("sh", abs, errno);
    _exit(1);
  }
  dup2(fd, want);
  close(fd);
}

/* Never returns: the child is this command, or it couldn't start. */
__attribute__((noreturn)) static void exec_cmd(struct cmd *c) {
  if (c->in != NULL)
    redirect(c->in, O_RDONLY, 0);
  if (c->out != NULL)
    redirect(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 1);

  absolutize(c);

  const char *name = c->argv[0];
  if (strchr(name, '/') != NULL) {
    char abs[PATH_LIMIT];
    if (resolve(name, abs, sizeof abs) == 0)
      execv(abs, c->argv);
    else
      errno = ENAMETOOLONG;
  } else {
    char path[PATH_LIMIT];
    if (strlen(name) + 6 < sizeof path) { /* "/bin/" and the NUL */
      strcpy(path, "/bin/");
      strcat(path, name);
      execv(path, c->argv);
    } else
      errno = ENAMETOOLONG;
  }
  complain("sh", name, errno);
  _exit(127);
}

/* Move the shell's cwd. With no argument, back to the root. */
static int do_cd(const struct cmd *c) {
  const char *where = c->argc > 1 ? c->argv[1] : "/";
  char abs[PATH_LIMIT];
  if (resolve(where, abs, sizeof abs) != 0) {
    complain("cd", where, ENAMETOOLONG);
    return 1;
  }
  struct stat st;
  if (stat(abs, &st) != 0) {
    complain("cd", where, errno);
    return 1;
  }
  if (st.type != FT_DIR) {
    complain("cd", where, ENOTDIR);
    return 1;
  }
  strlcpy(cwd, abs, sizeof cwd);
  return 0;
}

/*
 * The commands the shell has to run itself: exit, and the two that are
 * about its own cwd. Returns 1 if it handled the command, and leaves the
 * status in last_status.
 */
static int builtin(const struct cmd *c) {
  const char *name = c->argv[0];
  if (strcmp(name, "exit") == 0)
    _exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
  if (strcmp(name, "cd") == 0) {
    last_status = do_cd(c);
    return 1;
  }
  if (strcmp(name, "pwd") == 0) {
    printf("%s\n", cwd);
    last_status = 0;
    return 1;
  }
  return 0;
}

/* Run the pipeline and wait for all of it. Its status is the last
 * command's, as in sh. */
static int run_pipeline(struct cmd *cmd, int n) {
  int pids[MAX_CMDS];
  int prev_read = -1;
  int started = 0;

  for (int i = 0; i < n; i++) {
    int fds[2] = {-1, -1};
    if (i + 1 < n && pipe(fds) != 0) {
      complain("sh", "pipe", errno);
      break;
    }
    int pid = fork();
    if (pid < 0) {
      complain("sh", "fork", errno);
      if (fds[0] >= 0)
        close(fds[0]), close(fds[1]);
      break;
    }
    if (pid == 0) {
      if (prev_read >= 0) {
        dup2(prev_read, 0);
        close(prev_read);
      }
      if (fds[1] >= 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
      }
      exec_cmd(&cmd[i]);
    }
    pids[started++] = pid;
    if (prev_read >= 0)
      close(prev_read);
    if (fds[1] >= 0)
      close(fds[1]);
    prev_read = fds[0];
  }
  if (prev_read >= 0)
    close(prev_read);

  int status = 0;
  for (int left = started; left > 0; left--) {
    int st;
    int pid = wait(&st);
    if (pid < 0)
      break;
    if (pid == pids[started - 1])
      status = st;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* One line: parse it, run it, and report what it ended with. */
static int run_line(const char *line) {
  struct tok tok[MAX_TOKENS];
  struct cmd cmd[MAX_CMDS];
  char pool[LINE_MAX + MAX_TOKENS];

  if (strlen(line) >= LINE_MAX) {
    dprintf(2, "sh: line too long\n");
    return last_status = 2;
  }
  int ntok = lex(line, tok, MAX_TOKENS, pool);
  if (ntok < 0) {
    dprintf(2, "sh: cannot parse that line\n");
    return last_status = 2;
  }

  /* One pipeline at a time, up to the next ';' or '&&'. After '&&' the
   * rest of the line is skipped if the pipeline failed. */
  int skipping = 0;
  for (int i = 0; i <= ntok;) {
    int end = i;
    while (end < ntok && tok[end].type != T_SEMI && tok[end].type != T_AND)
      end++;

    if (!skipping) {
      int n = parse(&tok[i], end - i, cmd, MAX_CMDS);
      if (n < 0)
        return last_status = 2;
      if (n == 1 && builtin(&cmd[0]))
        ;
      else if (n > 0)
        last_status = run_pipeline(cmd, n);
    }
    if (end == ntok)
      break;
    skipping = tok[end].type == T_AND && (skipping || last_status != 0);
    if (tok[end].type == T_SEMI)
      skipping = 0;
    i = end + 1;
  }
  return last_status;
}

/* ---- reading lines -------------------------------------------------- */

/*
 * One line from `fd` into `buf` (without its newline): its length, or -1
 * at end of input. Reads a byte at a time, which the console gives us a
 * line at a time anyway.
 */
static int read_line(int fd, char *buf, size_t size) {
  size_t len = 0;
  for (;;) {
    char c;
    ssize_t r = read(fd, &c, 1);
    if (r < 0)
      return -1;
    if (r == 0)
      return len > 0 ? (int)len : -1;
    if (c == '\n')
      break;
    if (len + 1 < size)
      buf[len++] = c;
  }
  buf[len] = '\0';
  return (int)len;
}

static int repl(int fd, int prompt) {
  char line[LINE_MAX];
  for (;;) {
    if (prompt)
      printf("warden:%s$ ", cwd); /* where you are, since ls can't say */
    if (read_line(fd, line, sizeof line) < 0)
      return last_status;
    run_line(line);
  }
}

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "-c") == 0)
    return run_line(argv[2]);

  if (argc >= 2) {
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
      complain("sh", argv[1], errno);
      return 1;
    }
    int status = repl(fd, 0);
    close(fd);
    return status;
  }
  return repl(0, 1);
}
