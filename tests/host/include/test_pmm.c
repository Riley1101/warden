#include "check.h"
#include "pmm.h"

#include <abi/errno.h>
#include <stdlib.h>

/*
 * Fake physical memory: a host buffer standing in for RAM, with hhdm set so
 * that phys_to_virt(p) == mem + p. ASan catches anything outside it.
 */
#define MEM_SIZE (2UL * 1024 * 1024)
static uint8_t *mem;
#define HHDM ((uint64_t)(uintptr_t)mem)

#define NFRAMES (MEM_SIZE / PAGE_SIZE)

/* Allocate everything; check each frame is new, aligned, and in range.
 * Writes a pattern over each frame: if one overlapped the bitmap, the
 * PMM's own bookkeeping would break and later checks would fail. */
static size_t drain(uint64_t *out) {
  static uint8_t seen[NFRAMES];
  memset(seen, 0, sizeof seen);
  size_t n = 0;
  uint64_t p;
  while ((p = pmm_alloc()) != 0) {
    CHECK(p % PAGE_SIZE == 0);
    CHECK(p < MEM_SIZE);
    if (p >= MEM_SIZE)
      break;
    CHECK(!seen[p / PAGE_SIZE]);
    seen[p / PAGE_SIZE] = 1;
    memset(phys_to_virt(p), 0xAA, PAGE_SIZE);
    out[n++] = p;
  }
  return n;
}

static void test_basic(void) {
  /* low memory with a hole, like a PC: [4K, 636K) and [1M, 2M) */
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, HHDM) == 0);

  /* 158 + 256 frames; the bitmap (1 page) goes in the largest region */
  CHECK(pmm_free_count() == 158 + 256 - 1);

  uint64_t a = pmm_alloc();
  uint64_t b = pmm_alloc();
  CHECK(a != 0 && b != 0 && a != b);
  CHECK(pmm_free_count() == 158 + 256 - 3);
  CHECK(pmm_free(a) == 0);
  CHECK(pmm_free(b) == 0);
  CHECK(pmm_free_count() == 158 + 256 - 1);
}

static void test_exhaust(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, HHDM) == 0);
  size_t total = pmm_free_count();

  static uint64_t frames[NFRAMES];
  size_t n = drain(frames);
  CHECK(n == total);
  CHECK(pmm_free_count() == 0);
  CHECK(pmm_alloc() == 0);

  for (size_t i = 0; i < n; i++)
    CHECK(frames[i] != 0x100000); /* the bitmap's page */
  for (size_t i = 0; i < n; i++)
    CHECK(pmm_free(frames[i]) == 0);
  CHECK(pmm_free_count() == total);

  /* everything comes back: drain again gets the same count */
  CHECK(drain(frames) == total);
}

static void test_bad_free(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, HHDM) == 0);
  size_t total = pmm_free_count();

  uint64_t a = pmm_alloc();
  CHECK(pmm_free(a) == 0);
  CHECK(pmm_free(a) == -EINVAL);        /* double free */
  CHECK(pmm_free(a + 8) == -EINVAL);    /* unaligned */
  CHECK(pmm_free(0) == -EINVAL);        /* frame 0 */
  CHECK(pmm_free(0xa0000) == -EINVAL);  /* in the hole */
  CHECK(pmm_free(0x100000) == -EINVAL); /* the bitmap itself */
  CHECK(pmm_free(MEM_SIZE) == -EINVAL); /* past the top */
  CHECK(pmm_free(1UL << 40) == -EINVAL);
  CHECK(pmm_free_count() == total);     /* none of that changed anything */
}

static void test_frame_zero(void) {
  /* a region starting at 0: frame 0 must never be returned */
  struct pmm_region r[] = {{0x0, 0x4000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, HHDM) == 0);
  CHECK(pmm_free_count() == (4 - 1) + (256 - 1));
  static uint64_t frames[NFRAMES];
  size_t n = drain(frames);
  CHECK(n == (4 - 1) + (256 - 1));
}

static void test_unaligned_regions(void) {
  struct pmm_region r[] = {
      {0x1800, 0x2800},    /* -> [0x2000, 0x4000): 2 frames */
      {0x5100, 0x200},     /* less than a page: nothing */
      {0x10010, 0x4fff0},  /* -> [0x11000, 0x60000): 79 frames */
  };
  CHECK(pmm_init(r, 3, HHDM) == 0);
  CHECK(pmm_free_count() == 2 + 79 - 1);
  CHECK(pmm_free(0x1000) == -EINVAL); /* rounded off the front */
  CHECK(pmm_free(0x5000) == -EINVAL);
}

static void test_init_errors(void) {
  CHECK(pmm_init(NULL, 0, HHDM) == -ENOMEM);

  struct pmm_region tiny[] = {{0x5100, 0x200}};
  CHECK(pmm_init(tiny, 1, HHDM) == -ENOMEM); /* no whole page */

  /* top at 2 MiB needs a 1-page bitmap; a lone page is used up by it */
  struct pmm_region one[] = {{0x1ff000, 0x1000}};
  CHECK(pmm_init(one, 1, HHDM) == 0);
  CHECK(pmm_free_count() == 0);
  CHECK(pmm_alloc() == 0);

  static struct pmm_region many[PMM_MAX_REGIONS + 1];
  CHECK(pmm_init(many, PMM_MAX_REGIONS + 1, HHDM) == -EINVAL);
}

/* Random alloc/free against a model of which frames we hold. */
static void test_random(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, HHDM) == 0);
  size_t total = pmm_free_count();

  static uint64_t held[NFRAMES];
  static uint8_t in_use[NFRAMES];
  memset(in_use, 0, sizeof in_use);
  size_t nheld = 0;

  srand(1);
  for (int i = 0; i < 100000; i++) {
    if (nheld > 0 && (rand() % 3 == 0 || nheld == total)) {
      size_t k = (size_t)rand() % nheld;
      CHECK(pmm_free(held[k]) == 0);
      in_use[held[k] / PAGE_SIZE] = 0;
      held[k] = held[--nheld];
    } else {
      uint64_t p = pmm_alloc();
      CHECK(p != 0 && p < MEM_SIZE);
      if (p == 0 || p >= MEM_SIZE)
        return;
      CHECK(!in_use[p / PAGE_SIZE]); /* never hand out a held frame */
      in_use[p / PAGE_SIZE] = 1;
      held[nheld++] = p;
    }
    CHECK(pmm_free_count() == total - nheld);
  }
}

int main(void) {
  mem = calloc(1, MEM_SIZE);
  test_basic();
  test_exhaust();
  test_bad_free();
  test_frame_zero();
  test_unaligned_regions();
  test_init_errors();
  test_random();
  free(mem);
  return CHECK_DONE();
}
