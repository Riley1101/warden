#include "frame.h"
#include "policy.h"
#include "util.h"

#include <fcntl.h>
#include <sys/agent.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * The agent daemon: the guest end of /dev/agent, speaking the framed
 * protocol in frame.h.
 *
 * The host starts with a SESSION naming the configuration and profile,
 * then sends ACTIONs. For each, agentd runs `sh -c ACTION`, sends back
 * everything it printed (stdout and stderr together) in OUTPUT frames,
 * then a RESULT with the exit code. A command killed by a signal reports
 * 128 plus the signal, as a shell would. A frame agentd won't act on
 * gets an ERROR, and nothing runs.
 *
 * Configurations (paper, §5.1), for the profile the SESSION names:
 *   A  commands run as a human; the prompt is the only defence
 *   B  agentd checks each line against /etc/denylist first (policy.h)
 *   C  the child marks itself an agent before exec, so sh and all it
 *      starts carry the profile's label, and the kernel enforces it
 *   D  both
 * agentd itself always stays human.
 */

#define PROFILES      "/etc/profiles"
#define DENYLIST      "/etc/denylist"
#define AGENT_SESSION 1
#define WORK_DIR      "/home/work" /* where every profile's work happens */

static int agent;   /* the channel */
static char config; /* 'A' to 'D' once the SESSION came, else 0 */
static struct profile profile;
static struct denylist deny;

static int checks(void) { return config == 'B' || config == 'D'; }
static int marks(void) { return config == 'C' || config == 'D'; }

static void send(uint8_t type, const void *payload, uint32_t len) {
  uint8_t hdr[FRAME_HDR];
  frame_header(hdr, type, len);
  write_all(agent, (const char *)hdr, sizeof hdr);
  write_all(agent, payload, len);
}

static void send_error(const char *why) { send(F_ERROR, why, strlen(why)); }

/* "agentd: " and the formatted reason, as an ERROR. */
__attribute__((format(printf, 1, 2))) static void refuse(const char *fmt,
                                                         ...) {
  char why[256];
  va_list ap;
  va_start(ap, fmt);
  int n = snprintf(why, sizeof why, "agentd: ");
  vsnprintf(why + n, sizeof why - (size_t)n, fmt, ap);
  va_end(ap);
  send_error(why);
}

static void send_result(int code) {
  uint8_t le[4];
  frame_put32(le, (uint32_t)code);
  send(F_RESULT, le, sizeof le);
}

/* Run `line` under sh -c, its output sent as OUTPUT frames. */
static void run(const char *line) {
  int fds[2];
  if (pipe(fds) != 0) {
    send_error("agentd: pipe failed");
    return;
  }

  int pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    send_error("agentd: fork failed");
    return;
  }
  if (pid == 0) {
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    close(agent);
    if (marks() && mark_agent(AGENT_SESSION, profile.caps, profile.scope,
                              profile.max_procs) != 0) {
      complain("agentd", "mark_agent", errno);
      _exit(126); /* never run a command unlabeled */
    }
    char *const argv[] = {"sh", "-c", (char *)line, NULL};
    execv("/bin/sh", argv);
    _exit(127);
  }

  close(fds[1]);
  char buf[256];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0)
    send(F_OUTPUT, buf, (uint32_t)n);
  close(fds[0]);

  int status = 0;
  wait(&status);
  send_result(WIFEXITED(status) ? WEXITSTATUS(status)
                                : 128 + WTERMSIG(status));
}

/* All of `path` into `buf[size]`, terminated: 0, or an errno. */
static int slurp(const char *path, char *buf, size_t size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return errno;
  size_t len = 0;
  ssize_t n;
  while (len + 1 < size && (n = read(fd, buf + len, size - 1 - len)) > 0)
    len += (size_t)n;
  close(fd);
  buf[len] = '\0';
  return len + 1 < size ? 0 : EFBIG;
}

/* The profile `name`, and the deny-list if `with_deny`: 1, or 0 once
 * the reason it couldn't has gone to the host. */
static int load(const char *name, int with_deny) {
  static char text[4096];
  int e, found, line;
  if ((e = slurp(PROFILES, text, sizeof text)) != 0) {
    refuse("%s: %s", PROFILES, errname(e));
    return 0;
  }
  if ((found = profile_find(text, name, &profile)) < 0) {
    refuse("%s: line %d is not \"name caps scope max-procs\"", PROFILES,
           -found);
    return 0;
  }
  if (found == 0) {
    refuse("unknown profile %s", name);
    return 0;
  }
  if (!with_deny)
    return 1;
  if ((e = slurp(DENYLIST, text, sizeof text)) != 0) {
    refuse("%s: %s", DENYLIST, errname(e));
    return 0;
  }
  if ((line = deny_parse(text, &deny)) != 0) {
    refuse("%s: line %d is not a rule", DENYLIST, line);
    return 0;
  }
  return 1;
}

/* "<config> <profile>": set once, for the rest of the boot (a serial
 * line has no disconnect agentd could see to start over on). The files
 * are read here, not at start, so a mistake in them comes back as an
 * ERROR the host sees. */
static void session(const char *p) {
  if (config != 0)
    refuse("session already set");
  else if (p[0] < 'A' || p[0] > 'D' || p[1] != ' ')
    refuse("configuration must be A, B, C or D");
  else if (load(p + 2, p[0] == 'B' || p[0] == 'D')) {
    config = p[0];
    send_result(0);
  }
}

/* One frame from the host, answered with a RESULT or an ERROR. */
static void handle(const struct frame *f) {
  if (f->type != F_SESSION && f->type != F_ACTION) {
    send_error("agentd: unexpected frame type");
    return;
  }
  /* Both payloads are text: one NUL-terminated copy, refused if it
   * holds a NUL of its own (sh would see a shorter command). */
  static char text[FRAME_MAX + 1];
  memcpy(text, f->payload, f->len);
  text[f->len] = '\0';
  if (strlen(text) != f->len) {
    send_error("agentd: NUL in payload");
    return;
  }

  if (f->type == F_SESSION)
    session(text);
  else if (config == 0)
    send_error("agentd: no session");
  else {
    char word[POLICY_WORD];
    const char *why = checks() ? deny_check(&deny, &profile, text, word) : NULL;
    if (why != NULL)
      refuse("deny-list: %s: %s", why, word);
    else
      run(text);
  }
}

int main(void) {
  agent = open("/dev/agent", O_RDWR);
  if (agent < 0) {
    complain("agentd", "/dev/agent", errno);
    return 1;
  }

  /* The scope has to exist for the agent to work in it. */
  if ((mkdir("/home") != 0 && errno != EEXIST) ||
      (mkdir(WORK_DIR) != 0 && errno != EEXIST)) {
    complain("agentd", WORK_DIR, errno);
    return 1;
  }

  static struct frame_reader r;
  for (;;) {
    size_t room;
    uint8_t *space = frame_space(&r, &room);
    ssize_t n = read(agent, space, room);
    if (n <= 0)
      return 1;
    frame_added(&r, (size_t)n);

    struct frame f;
    int k;
    while ((k = frame_next(&r, &f)) != 0) {
      if (k < 0)
        send_error("agentd: frame too long");
      else
        handle(&f);
    }
  }
}
