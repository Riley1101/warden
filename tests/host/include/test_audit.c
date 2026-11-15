#include "audit.h"
#include "check.h"

#include <stdlib.h>

/* A record whose pid says which it was, to check order by. */
static void log_pid(int pid) {
  struct audit_record r = {.pid = pid, .kind = AUDIT_DENIED};
  audit_log(&r);
}

static void test_empty(void) {
  struct audit_record out[4];
  CHECK(audit_read(out, 4, 0) == 0);
}

static void test_in_order(void) {
  struct audit_record out[8];
  for (int i = 1; i <= 3; i++)
    log_pid(i);
  CHECK(audit_read(out, 8, 0) == 3);
  for (int i = 0; i < 3; i++) {
    CHECK(out[i].seq == (uint64_t)i + 1);
    CHECK(out[i].pid == i + 1);
  }
  /* after skips what was seen; max stops early */
  CHECK(audit_read(out, 8, 2) == 1 && out[0].seq == 3);
  CHECK(audit_read(out, 1, 0) == 1 && out[0].seq == 1);
  CHECK(audit_read(out, 8, 3) == 0);
  CHECK(audit_read(out, 8, 1000) == 0); /* from the future: nothing yet */
  CHECK(audit_read(out, 0, 0) == 0);
}

/* Past capacity: the oldest go, seq keeps counting, nothing mixes. */
static void test_wraparound(void) {
  struct audit_record *out = calloc(AUDIT_RING, sizeof *out);
  int total = 3 + 2 * AUDIT_RING + 5; /* three from before, then wrap twice */
  for (int i = 4; i <= total; i++)
    log_pid(i);

  uint64_t oldest = (uint64_t)total - AUDIT_RING + 1;
  CHECK(audit_read(out, AUDIT_RING, 0) == AUDIT_RING);
  CHECK(out[0].seq == oldest);
  for (size_t i = 0; i < AUDIT_RING; i++) {
    CHECK(out[i].seq == oldest + i);
    CHECK(out[i].pid == (int)(oldest + i)); /* the record, not a stale one */
  }

  /* a reader that fell behind lands on the oldest left: the jump shows */
  CHECK(audit_read(out, 1, 5) == 1 && out[0].seq == oldest);
  /* one that kept up gets just the rest */
  CHECK(audit_read(out, AUDIT_RING, (uint64_t)total - 2) == 2);
  CHECK(out[0].seq == (uint64_t)total - 1 && out[1].seq == (uint64_t)total);
  free(out);
}

/* The seq is the ring's to give, whatever the caller left in it. */
static void test_seq_ignored(void) {
  struct audit_record r = {.seq = 12345, .pid = -1}, out;
  audit_log(&r);
  uint64_t last = 3 + 2 * AUDIT_RING + 5;
  CHECK(audit_read(&out, 1, last) == 1);
  CHECK(out.seq == last + 1 && out.pid == -1);
}

int main(void) {
  test_empty();
  test_in_order();
  test_wraparound();
  test_seq_ignored();
  return CHECK_DONE();
}
