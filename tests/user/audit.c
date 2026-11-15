#include "t.h"

#include <sys/agent.h>
#include <sys/audit.h>

/*
 * The audit log, from ring 3: what an agent is refused shows up for a
 * human to read, and the agent can't read it. The log is shared with
 * every test before this one, so this reads only what comes after the
 * last record already there.
 */

#define SESSION 7

static uint64_t last_seq(void) {
  struct audit_record r[4];
  uint64_t seq = 0;
  int n;
  while ((n = audit_read(r, 4, seq)) > 0)
    seq = r[n - 1].seq;
  return n == 0 ? seq : (uint64_t)-1;
}

/* Run `fn` in a child marked with `caps`; its pid once it has exited 0,
 * else -1. */
static int as_agent(uint32_t caps, int (*fn)(void)) {
  int pid = fork();
  if (pid == 0) {
    if (mark_agent(SESSION, caps, "/home/work") != 0)
      _exit(99);
    _exit(fn());
  }
  int status;
  if (pid < 0 || wait(&status) != pid || status != 0)
    return -1;
  return pid;
}

static int escape(void) {
  if (!FAILS(unlink("/audit/victim"), ECAPDENIED))
    return 1;
  if (!FAILS(mark_agent(SESSION, CAP_FS_READ, "/"), EPERM)) /* widening */
    return 2;
  return 0;
}

static int snoop(void) {
  struct audit_record r;
  return FAILS(audit_read(&r, 1, 0), ECAPDENIED) ? 0 : 1;
}

/* The next record, which must be `kind` from `pid` in `syscall`. */
static int expect(struct audit_record *r, uint64_t *seq, uint32_t kind,
                  int pid, const char *syscall) {
  if (audit_read(r, 1, *seq) != 1)
    return 0;
  *seq = r->seq;
  return r->kind == kind && r->pid == pid && r->session == SESSION &&
         strcmp(r->syscall, syscall) == 0;
}

int main(void) {
  uint64_t seq = last_seq();
  if (seq == (uint64_t)-1)
    return 1;

  /* a human reading leaves no record of its own */
  if (last_seq() != seq)
    return 2;

  int a = as_agent(CAP_FS_READ, escape);
  int b = as_agent(CAP_FS_READ, snoop);
  if (a < 0 || b < 0)
    return 3;

  struct audit_record r;
  if (!expect(&r, &seq, AUDIT_MARK, a, "mark_agent") ||
      r.need != CAP_FS_READ || strcmp(r.path, "/home/work") != 0)
    return 4;
  /* the path as the agent gave it, and what it needed */
  if (!expect(&r, &seq, AUDIT_DENIED, a, "unlink") ||
      r.err != -ECAPDENIED || r.need != CAP_FS_WRITE ||
      strcmp(r.path, "/audit/victim") != 0)
    return 5;
  if (!expect(&r, &seq, AUDIT_DENIED, a, "mark_agent") || r.err != -EPERM ||
      strcmp(r.path, "/") != 0)
    return 6;
  if (!expect(&r, &seq, AUDIT_MARK, b, "mark_agent"))
    return 7;
  /* reading the log is refused, and that is logged too */
  if (!expect(&r, &seq, AUDIT_DENIED, b, "audit_read") ||
      r.err != -ECAPDENIED || r.path[0] != '\0')
    return 8;
  if (audit_read(&r, 1, seq) != 0)
    return 9; /* nothing else */

  /* the kernel hands back a few per call, and a bad buffer is EFAULT */
  if (!FAILS(audit_read((struct audit_record *)8, 1, 0), EFAULT))
    return 10;
  return 0;
}
