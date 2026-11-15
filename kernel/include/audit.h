#pragma once

#include <abi/audit.h>
#include <stddef.h>

/*
 * The audit ring: the newest AUDIT_RING records, older ones overwritten.
 * Callers keep interrupts off (every syscall runs that way), so it needs
 * no lock.
 *
 * Must stay free of hardware code: it is compiled into the kernel AND
 * into host unit tests with sanitizers.
 */

#define AUDIT_RING 256

/* Add a copy of `r`, numbered with the next seq (r->seq is ignored). */
void audit_log(const struct audit_record *r);

/* Up to `max` records with seq after `after`, oldest first, into `out`.
 * How many. Those already overwritten are skipped: the seq jumps. */
size_t audit_read(struct audit_record *out, size_t max, uint64_t after);
