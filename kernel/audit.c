#include "audit.h"

static struct audit_record ring[AUDIT_RING];
static uint64_t next_seq = 1; /* the seq the next record gets */

void audit_log(const struct audit_record *r) {
  struct audit_record *slot = &ring[next_seq % AUDIT_RING];
  *slot = *r;
  slot->seq = next_seq++;
}

size_t audit_read(struct audit_record *out, size_t max, uint64_t after) {
  uint64_t oldest = next_seq > AUDIT_RING ? next_seq - AUDIT_RING : 1;
  uint64_t seq = after + 1 > oldest ? after + 1 : oldest;
  size_t n = 0;
  for (; n < max && seq < next_seq; seq++)
    out[n++] = ring[seq % AUDIT_RING];
  return n;
}
