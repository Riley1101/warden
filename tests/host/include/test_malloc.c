#include "check.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h> /* malloc & co. are lc_malloc & co. here */
#include <unistd.h>

/* The fake heap sbrk hands out (malloc here is the code under test, so the
 * arena can't come from it). ASan flags accesses past its end. */
#define ARENA (4 * 1024 * 1024)
static char arena[ARENA] __attribute__((aligned(16)));
static size_t arena_used, arena_cap = ARENA;

void *lc_sbrk(intptr_t inc) {
  if (inc < 0 || arena_used + (size_t)inc > arena_cap) {
    errno = ENOMEM;
    return (void *)-1;
  }
  void *old = arena + arena_used;
  arena_used += (size_t)inc;
  return old;
}

static int bugs;
static const char *last_bug;
void malloc_bug(const char *what, const void *p) {
  (void)p;
  bugs++;
  last_bug = what;
}

static void test_basic(void) {
  char *a = malloc(1), *b = malloc(100), *c = malloc(0);
  CHECK(a && b && c);
  CHECK((uintptr_t)a % 16 == 0 && (uintptr_t)b % 16 == 0);
  CHECK(a != b && b != c && a != c);
  memset(b, 'x', 100);
  free(a);
  free(c);
  free(NULL);
  char *d = malloc(1); /* first fit: reuses a freed block */
  CHECK(d == a || d == c);
  free(b);
  free(d);
  CHECK(bugs == 0);
}

static void test_calloc_realloc(void) {
  int *z = calloc(1000, sizeof *z);
  CHECK(z != NULL);
  int nonzero = 0;
  for (int i = 0; i < 1000; i++)
    nonzero |= z[i];
  CHECK(nonzero == 0);
  free(z);

  errno = 0;
  CHECK(calloc(SIZE_MAX / 2, 4) == NULL && errno == ENOMEM); /* overflow */
  CHECK(malloc(SIZE_MAX) == NULL);

  char *p = realloc(NULL, 10);
  strcpy(p, "keep me");
  p = realloc(p, 5000); /* grows: moves, keeps the contents */
  CHECK(p != NULL && strcmp(p, "keep me") == 0);
  CHECK(realloc(p, 8) == p); /* shrinking stays put */
  CHECK(realloc(p, 0) == NULL); /* frees */
  CHECK(bugs == 0);
}

static void test_bad_frees(void) {
  char *p = malloc(32);
  free(p);
  free(p);
  CHECK(bugs == 1 && strcmp(last_bug, "double free") == 0);
  int on_stack;
  free(&on_stack);
  CHECK(bugs == 2 && strcmp(last_bug, "not a malloc'd pointer") == 0);
  char *q = malloc(64);
  free(q + 16); /* interior pointer */
  CHECK(bugs == 3);
  free(q + 1); /* misaligned */
  CHECK(bugs == 4);
  free(q);
  bugs = 0;
}

/* Out of memory: NULL with errno, and the heap still works after. */
static void test_oom(void) {
  size_t cap = arena_cap;
  arena_cap = arena_used; /* no more growth */
  errno = 0;
  CHECK(malloc(ARENA) == NULL && errno == ENOMEM);
  arena_cap = cap;
  void *p = malloc(64);
  CHECK(p != NULL);
  free(p);
}

/* Random malloc/free/realloc against a model: each live block is filled
 * with its own byte, so overlap or corruption shows. */
#define SLOTS 512
static void test_random(void) {
  static char *ptr[SLOTS];
  static size_t len[SLOTS];
  srand(3);
  for (int i = 0; i < 200000; i++) {
    int s = rand() % SLOTS;
    if (ptr[s] != NULL) {
      for (size_t k = 0; k < len[s]; k++)
        if (ptr[s][k] != (char)s) {
          CHECK(!"block corrupted");
          return;
        }
    }
    int op = rand() % 3;
    size_t n = (size_t)(rand() % 4 == 0 ? rand() % 9000 : rand() % 200);
    if (op == 0 || ptr[s] == NULL) {
      free(ptr[s]);
      ptr[s] = malloc(n);
      len[s] = n;
    } else if (op == 1) {
      free(ptr[s]);
      ptr[s] = NULL;
      len[s] = 0;
      continue;
    } else {
      ptr[s] = realloc(ptr[s], n);
      len[s] = n;
      if (n == 0)
        continue;
    }
    CHECK(ptr[s] != NULL);
    memset(ptr[s], (char)s, n);
  }
  for (int s = 0; s < SLOTS; s++)
    free(ptr[s]);
  CHECK(bugs == 0);

  /* Everything free again: it all merged back, so one big block fits
   * without growing the heap. */
  size_t used = arena_used;
  void *big = malloc(used - 64 * 1024);
  CHECK(big != NULL && arena_used == used);
  free(big);
}

int main(void) {
  test_basic();
  test_calloc_realloc();
  test_bad_frees();
  test_oom();
  test_random();
  return CHECK_DONE();
}
