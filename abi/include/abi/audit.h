#pragma once

#include <stdint.h>

/*
 * The audit log: what the kernel refused an agent, and who became one.
 * The kernel keeps the newest records in a ring; SYS_audit_read hands
 * them to a human process (an agent asking is itself refused, and
 * logged). `seq` counts every record ever made, from 1, so a gap
 * between two a reader sees is how many the ring dropped in between.
 */

#define AUDIT_DENIED 0 /* a call, or a path in one, was refused */
#define AUDIT_MARK   1 /* mark_agent succeeded: `need` holds the caps given */
#define AUDIT_KILL   2 /* kill_session ended `session`, by a human */

#define AUDIT_NAME 16
#define AUDIT_PATH 256

struct audit_record {
  uint64_t seq;
  int32_t pid;
  uint32_t session;
  uint32_t kind;             /* AUDIT_DENIED or AUDIT_MARK */
  uint32_t need;             /* CAP_* bits asked for (abi/cap.h) */
  int32_t err;               /* what the call returned: -ECAPDENIED, -EPERM */
  char syscall[AUDIT_NAME];  /* "unlink" */
  char path[AUDIT_PATH];     /* as the caller gave it; mark: the scope */
};
