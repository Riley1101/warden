#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A first-fit heap over sbrk. Every block is a 16-byte header, then its
 * payload (a multiple of 16 bytes). Free blocks sit on a list in address
 * order, so a freed block merges with free neighbours on both sides.
 *
 * Also host-tested (tests/host/include/test_malloc.c): there, a rename
 * header makes these lc_malloc & co. over a fake sbrk.
 */

void malloc_bug(const char *what, const void *p); /* stdlib.c, or the test */

struct hdr {
  size_t size; /* payload bytes */
  size_t tag;  /* TAG_USED or TAG_FREE: a stray free hits neither */
};

#define ALIGN    16
#define TAG_USED 0xa110ca7eda110ca7UL
#define TAG_FREE 0xf4eeb10cf4eeb10cUL
#define CHUNK    (64 * 1024) /* grow the heap at least this much at once */

_Static_assert(sizeof(struct hdr) == ALIGN, "header keeps payloads aligned");

static struct hdr *free_list; /* address order */
static char *heap_lo, *heap_hi; /* everything sbrk gave us */

static char *end_of(struct hdr *h) { return (char *)(h + 1) + h->size; }
static struct hdr **next_of(struct hdr *h) { return (struct hdr **)(h + 1); }

/* Put `h` on the free list, merging with the blocks either side. */
static void insert_free(struct hdr *h) {
  h->tag = TAG_FREE;
  struct hdr *prev = NULL, *cur = free_list;
  while (cur != NULL && cur < h) {
    prev = cur;
    cur = *next_of(cur);
  }
  *next_of(h) = cur;
  if (cur != NULL && end_of(h) == (char *)cur) {
    h->size += sizeof *h + cur->size;
    *next_of(h) = *next_of(cur);
    cur->tag = 0;
  }
  if (prev == NULL) {
    free_list = h;
  } else if (end_of(prev) == (char *)h) {
    prev->size += sizeof *h + h->size;
    *next_of(prev) = *next_of(h);
    h->tag = 0;
  } else {
    *next_of(prev) = h;
  }
}

static int grow(size_t n) {
  size_t want = n + sizeof(struct hdr);
  want = want < CHUNK ? CHUNK : (want + CHUNK - 1) / CHUNK * CHUNK;
  char *p = sbrk((intptr_t)want);
  if (p == (char *)-1)
    return -1;
  if (heap_lo == NULL)
    heap_lo = p;
  heap_hi = p + want;
  struct hdr *h = (struct hdr *)p;
  h->size = want - sizeof *h;
  insert_free(h);
  return 0;
}

void *malloc(size_t n) {
  if (n > SIZE_MAX / 2) {
    errno = ENOMEM;
    return NULL;
  }
  n = n == 0 ? ALIGN : (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
  for (int tries = 0; tries < 2; tries++) {
    struct hdr **link = &free_list;
    for (struct hdr *h = free_list; h != NULL; h = *link) {
      if (h->size < n) {
        link = next_of(h);
        continue;
      }
      if (h->size - n >= sizeof *h + ALIGN) { /* split: the rest stays */
        struct hdr *rest = (struct hdr *)((char *)(h + 1) + n);
        rest->size = h->size - n - sizeof *rest;
        rest->tag = TAG_FREE;
        *next_of(rest) = *next_of(h);
        *link = rest;
        h->size = n;
      } else {
        *link = *next_of(h);
      }
      h->tag = TAG_USED;
      return h + 1;
    }
    if (grow(n) != 0)
      return NULL; /* sbrk set errno */
  }
  return NULL; /* not reached: a fresh chunk always fits */
}

/* The header of a block malloc gave out, or malloc_bug. */
static struct hdr *used_block(void *p) {
  if (heap_lo == NULL || (char *)p < heap_lo + sizeof(struct hdr) ||
      (char *)p >= heap_hi || (uintptr_t)p % ALIGN != 0) {
    malloc_bug("not a malloc'd pointer", p);
    return NULL;
  }
  struct hdr *h = (struct hdr *)p - 1;
  if (h->tag != TAG_USED) {
    malloc_bug(h->tag == TAG_FREE ? "double free" : "not a malloc'd pointer",
               p);
    return NULL;
  }
  return h;
}

void free(void *p) {
  if (p == NULL)
    return;
  struct hdr *h = used_block(p);
  if (h != NULL)
    insert_free(h);
}

void *realloc(void *p, size_t n) {
  if (p == NULL)
    return malloc(n);
  if (n == 0) {
    free(p);
    return NULL;
  }
  struct hdr *h = used_block(p);
  if (h == NULL)
    return NULL;
  if (h->size >= n)
    return p;
  void *q = malloc(n);
  if (q == NULL)
    return NULL; /* p is untouched */
  memcpy(q, p, h->size);
  free(p);
  return q;
}

void *calloc(size_t count, size_t size) {
  if (size != 0 && count > SIZE_MAX / size) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = malloc(count * size);
  if (p != NULL)
    memset(p, 0, count * size);
  return p;
}
