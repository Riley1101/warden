#pragma once

#include <abi/audit.h> /* struct audit_record, AUDIT_DENIED & co. */
#include <stddef.h>
#include <stdint.h>

/* Up to `max` audit records with seq after `after`, oldest first: how
 * many (0 when caught up), or -1 and errno. The kernel hands back a few
 * per call, so loop with the last seq seen. ECAPDENIED for an agent. */
int audit_read(struct audit_record *buf, size_t max, uint64_t after);
