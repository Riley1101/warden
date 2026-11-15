#include "util.h"

#include <sys/agent.h>
#include <sys/audit.h>

/*
 * audit: print the audit log, oldest first. A gap in SEQ is records the
 * kernel's ring dropped. NEED is the capabilities asked for (on a mark,
 * those given): r read, w write, s spawn, k kill, H human only.
 */

static void caps(char *out, uint32_t c) {
  const char *letters = "rwsk";
  int n = 0;
  for (int i = 0; i < 4; i++)
    if (c & (1u << i))
      out[n++] = letters[i];
  if (c & (1u << 30)) /* CAP_HUMAN, kernel-side */
    out[n++] = 'H';
  if (n == 0)
    out[n++] = '-';
  out[n] = '\0';
}

int main(void) {
  printf("%5s %5s %4s %-6s %-11s %-4s %-11s %s\n", "SEQ", "PID", "SESS",
         "KIND", "CALL", "NEED", "RESULT", "PATH");
  struct audit_record r[16];
  uint64_t seq = 0;
  int n;
  while ((n = audit_read(r, 16, seq)) > 0) {
    for (int i = 0; i < n; i++) {
      char need[8];
      caps(need, r[i].need);
      printf("%5d %5d %4d %-6s %-11s %-4s %-11s %s\n", (int)r[i].seq,
             r[i].pid, (int)r[i].session,
             r[i].kind == AUDIT_MARK ? "mark" : "denied", r[i].syscall, need,
             r[i].err == 0 ? "ok" : errname(-r[i].err), r[i].path);
    }
    seq = r[n - 1].seq;
  }
  if (n < 0) {
    complain("audit", "audit_read", errno);
    return 1;
  }
  return 0;
}
