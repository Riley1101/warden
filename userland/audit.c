#include "util.h"

#include <sys/agent.h>
#include <sys/audit.h>

/*
 * audit: print the audit log, oldest first. A gap in SEQ is records the
 * kernel's ring dropped. NEED is the capabilities asked for (on a mark,
 * those given), as caps_str spells them: r read, w write, s spawn,
 * k kill, '-' for each one not in it. "HUMAN" is a humans-only call.
 */

#define CAP_HUMAN (1u << 30) /* kernel-side (kernel/include/cap.h) */

static void need_str(char *out, uint32_t c) {
  if (c & CAP_HUMAN)
    strcpy(out, "HUMAN");
  else
    caps_str(out, c);
}

static const char *kind_str(uint32_t kind) {
  return kind == AUDIT_MARK ? "mark" : kind == AUDIT_KILL ? "kill" : "denied";
}

int main(void) {
  printf("%5s %5s %4s %-6s %-12s %-5s %-11s %s\n", "SEQ", "PID", "SESS",
         "KIND", "CALL", "NEED", "RESULT", "PATH");
  struct audit_record r[16];
  uint64_t seq = 0;
  int n;
  while ((n = audit_read(r, 16, seq)) > 0) {
    for (int i = 0; i < n; i++) {
      char need[8];
      need_str(need, r[i].need);
      printf("%5d %5d %4d %-6s %-12s %-5s %-11s %s\n", (int)r[i].seq,
             r[i].pid, (int)r[i].session,
             kind_str(r[i].kind), r[i].syscall, need,
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
