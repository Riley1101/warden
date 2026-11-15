#include "check.h"
#include "frame.h"

/* `n` bytes into the reader, as if read from the channel. */
static void feed(struct frame_reader *r, const void *p, size_t n) {
  size_t room;
  uint8_t *dst = frame_space(r, &room);
  CHECK(n <= room);
  memcpy(dst, p, n);
  frame_added(r, n);
}

/* A whole frame's bytes into `out`; how many. */
static size_t make(uint8_t *out, uint8_t type, const char *payload,
                   uint32_t len) {
  frame_header(out, type, len);
  memcpy(out + FRAME_HDR, payload, len);
  return FRAME_HDR + len;
}

static void test_whole(void) {
  struct frame_reader r = {0};
  struct frame f;
  uint8_t b[64];
  CHECK(frame_next(&r, &f) == 0); /* nothing yet */

  size_t n = make(b, F_ACTION, "ls /bin", 7);
  feed(&r, b, n);
  CHECK(frame_next(&r, &f) == 1);
  CHECK(f.type == F_ACTION && f.len == 7);
  CHECK(memcmp(f.payload, "ls /bin", 7) == 0);
  CHECK(frame_next(&r, &f) == 0);
  CHECK(r.have == 0); /* consumed */

  /* an empty payload is a frame too */
  feed(&r, b, make(b, F_SESSION, "", 0));
  CHECK(frame_next(&r, &f) == 1 && f.type == F_SESSION && f.len == 0);
}

/* However the bytes are split, the same frames come out. */
static void test_split(void) {
  uint8_t b[128];
  size_t n = make(b, F_SESSION, "C work-rw", 9);
  n += make(b + n, F_ACTION, "echo hi", 7);
  for (size_t step = 1; step <= n; step++) {
    struct frame_reader r = {0};
    struct frame f;
    int got = 0;
    for (size_t off = 0; off < n; off += step) {
      feed(&r, b + off, off + step <= n ? step : n - off);
      int k;
      while ((k = frame_next(&r, &f)) == 1) {
        if (got == 0)
          CHECK(f.type == F_SESSION && f.len == 9 &&
                memcmp(f.payload, "C work-rw", 9) == 0);
        else
          CHECK(f.type == F_ACTION && f.len == 7 &&
                memcmp(f.payload, "echo hi", 7) == 0);
        got++;
      }
      CHECK(k == 0);
    }
    CHECK(got == 2);
  }
}

/* A frame longer than allowed is refused, skipped, and the next one
 * still comes through, even when the long one arrives in pieces. */
static void test_too_long(void) {
  struct frame_reader r = {0};
  struct frame f;
  uint8_t hdr[FRAME_HDR], junk[1000], b[64];
  memset(junk, 'x', sizeof junk);
  uint32_t len = FRAME_MAX + 2500;
  frame_header(hdr, F_ACTION, len);
  feed(&r, hdr, sizeof hdr);
  CHECK(frame_next(&r, &f) == -1);
  for (uint32_t sent = 0; sent < len; sent += sizeof junk) {
    uint32_t k = len - sent < sizeof junk ? len - sent : sizeof junk;
    feed(&r, junk, k);
    CHECK(frame_next(&r, &f) == 0);
  }
  CHECK(r.skip == 0 && r.have == 0);
  feed(&r, b, make(b, F_ACTION, "pwd", 3));
  CHECK(frame_next(&r, &f) == 1 && f.len == 3);

  /* the largest allowed one fits the buffer whole */
  static uint8_t big[FRAME_HDR + FRAME_MAX];
  frame_header(big, F_ACTION, FRAME_MAX);
  memset(big + FRAME_HDR, 'y', FRAME_MAX);
  struct frame_reader r2 = {0};
  feed(&r2, big, sizeof big);
  CHECK(frame_next(&r2, &f) == 1 && f.len == FRAME_MAX);

  /* a length past 4 GiB's worth of skipping still counts right */
  struct frame_reader r3 = {0};
  uint8_t max[FRAME_HDR] = {0xff, 0xff, 0xff, 0xff, F_ACTION};
  feed(&r3, max, sizeof max);
  CHECK(frame_next(&r3, &f) == -1);
  CHECK(r3.skip == (uint64_t)FRAME_HDR + 0xffffffffu); /* no wrap */
}

static void test_header(void) {
  uint8_t h[FRAME_HDR];
  frame_header(h, F_RESULT, 0x01020304);
  CHECK(h[0] == 4 && h[1] == 3 && h[2] == 2 && h[3] == 1 && h[4] == F_RESULT);
  CHECK(frame_le32(h) == 0x01020304);
}

int main(void) {
  test_whole();
  test_split();
  test_too_long();
  test_header();
  return CHECK_DONE();
}
