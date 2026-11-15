#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The agent protocol: frames over /dev/agent, between agentd and the host
 * (host/channel.py speaks the other end).
 *
 *   [u32 len, little-endian][u8 type][len bytes of payload]
 *
 * The host sends one SESSION, then ACTIONs. agentd answers every frame
 * the host sends with either a RESULT (an ACTION's output comes first,
 * in OUTPUT frames) or one ERROR, never both and never neither:
 *
 *   SESSION  host -> guest  "<config> <profile>", e.g. "C work-rw"
 *   ACTION   host -> guest  one command line for sh -c
 *   OUTPUT   guest -> host  some of the command's stdout and stderr
 *   RESULT   guest -> host  u32 little-endian: the exit code (0 for SESSION)
 *   ERROR    guest -> host  why the frame was refused; nothing ran
 *
 * A frame longer than FRAME_MAX is refused and its bytes skipped, so the
 * stream stays in step with an honest sender whatever the frame held.
 *
 * Pure, so the parser is host-tested (tests/host/include/test_frame.c).
 */

#define FRAME_HDR 5
#define FRAME_MAX 4096 /* payload bytes, either way */

enum frame_type {
  F_SESSION = 1,
  F_ACTION = 2,
  F_OUTPUT = 3,
  F_RESULT = 4,
  F_ERROR = 5,
};

struct frame {
  uint8_t type;
  const uint8_t *payload; /* into the reader's buffer, until the next call */
  uint32_t len;
};

/* Bytes from the channel, cut into frames however they arrived. */
struct frame_reader {
  uint8_t buf[FRAME_HDR + FRAME_MAX];
  size_t have;   /* bytes in buf */
  size_t used;   /* the frame last returned, dropped on the next call */
  uint64_t skip; /* bytes still to throw away, of a frame too long */
};

/* Where the next bytes read go, and how many fit. */
__attribute__((unused)) static uint8_t *frame_space(struct frame_reader *r,
                                                    size_t *room) {
  *room = sizeof r->buf - r->have;
  return r->buf + r->have;
}

__attribute__((unused)) static void frame_added(struct frame_reader *r,
                                                size_t n) {
  r->have += n;
}

static void frame_drop(struct frame_reader *r, size_t n) {
  memmove(r->buf, r->buf + n, r->have - n);
  r->have -= n;
}

static uint32_t frame_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

/*
 * The next whole frame into *f: 1. 0 when more bytes are needed. -1 for
 * a frame longer than FRAME_MAX, which is then skipped as its bytes come.
 */
__attribute__((unused)) static int frame_next(struct frame_reader *r,
                                              struct frame *f) {
  if (r->used > 0) {
    frame_drop(r, r->used);
    r->used = 0;
  }
  if (r->skip > 0) {
    size_t k = r->skip < r->have ? (size_t)r->skip : r->have;
    frame_drop(r, k);
    r->skip -= k;
    if (r->skip > 0)
      return 0;
  }
  if (r->have < FRAME_HDR)
    return 0;
  uint32_t len = frame_le32(r->buf);
  if (len > FRAME_MAX) {
    r->skip = (uint64_t)FRAME_HDR + len;
    return -1;
  }
  if (r->have < FRAME_HDR + (size_t)len)
    return 0;
  f->type = r->buf[4];
  f->payload = r->buf + FRAME_HDR;
  f->len = len;
  r->used = FRAME_HDR + len;
  return 1;
}

static void frame_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* The header of a frame of `type` carrying `len` bytes, into `out`. */
__attribute__((unused)) static void frame_header(uint8_t out[FRAME_HDR],
                                                 uint8_t type, uint32_t len) {
  frame_put32(out, len);
  out[4] = type;
}
