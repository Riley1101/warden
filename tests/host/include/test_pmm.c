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

/* Pages of metadata (bitmap + struct page array) for memory [0, top). */
static size_t meta(uint64_t top) {
  size_t frames = top / PAGE_SIZE;
  size_t bitmap = ((frames + 63) / 64 * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
  size_t array = (frames * sizeof(struct page) + PAGE_SIZE - 1) / PAGE_SIZE;
  return bitmap + array;
}

/* Allocate everything; check each frame is new, aligned, and in range.
 * Writes a pattern over each frame: if one overlapped the metadata, the
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
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);

  /* 158 + 256 frames; the metadata goes in the largest region */
  size_t m = meta(0x200000);
  CHECK(m == 1 + 4); /* 512 frames: 64-byte bitmap, 16 KiB of struct page */
  CHECK(pmm_free_count() == 158 + 256 - m);

  uint64_t a = pmm_alloc();
  uint64_t b = pmm_alloc();
  CHECK(a != 0 && b != 0 && a != b);
  CHECK(pmm_free_count() == 158 + 256 - m - 2);
  CHECK(pmm_free(a) == 0);
  CHECK(pmm_free(b) == 0);
  CHECK(pmm_free_count() == 158 + 256 - m);
}

static void test_exhaust(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);
  size_t total = pmm_free_count();

  static uint64_t frames[NFRAMES];
  size_t n = drain(frames);
  CHECK(n == total);
  CHECK(pmm_free_count() == 0);
  CHECK(pmm_alloc() == 0);

  for (size_t i = 0; i < n; i++) /* never the metadata's pages */
    CHECK(frames[i] < 0x100000 ||
          frames[i] >= 0x100000 + meta(0x200000) * PAGE_SIZE);
  for (size_t i = 0; i < n; i++)
    CHECK(pmm_free(frames[i]) == 0);
  CHECK(pmm_free_count() == total);

  /* everything comes back: drain again gets the same count */
  CHECK(drain(frames) == total);
}

static void test_bad_free(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);
  size_t total = pmm_free_count();

  uint64_t a = pmm_alloc();
  CHECK(pmm_free(a) == 0);
  CHECK(pmm_free(a) == -EINVAL);        /* double free */
  CHECK(pmm_free(a + 8) == -EINVAL);    /* unaligned */
  CHECK(pmm_free(0) == -EINVAL);        /* frame 0 */
  CHECK(pmm_free(0xa0000) == -EINVAL);  /* in the hole */
  CHECK(pmm_free(0x100000) == -EINVAL); /* the metadata itself */
  CHECK(pmm_free(0x104000) == -EINVAL); /* ... all of it */
  CHECK(pmm_free(MEM_SIZE) == -EINVAL); /* past the top */
  CHECK(pmm_free(1UL << 40) == -EINVAL);
  CHECK(pmm_free_count() == total);     /* none of that changed anything */
}

static void test_frame_zero(void) {
  /* a region starting at 0: frame 0 must never be returned */
  struct pmm_region r[] = {{0x0, 0x4000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);
  size_t want = (4 - 1) + (256 - meta(0x200000));
  CHECK(pmm_free_count() == want);
  static uint64_t frames[NFRAMES];
  size_t n = drain(frames);
  CHECK(n == want);
}

static void test_unaligned_regions(void) {
  struct pmm_region r[] = {
      {0x1800, 0x2800},    /* -> [0x2000, 0x4000): 2 frames */
      {0x5100, 0x200},     /* less than a page: nothing */
      {0x10010, 0x4fff0},  /* -> [0x11000, 0x60000): 79 frames */
  };
  CHECK(pmm_init(r, 3, 0, HHDM) == 0);
  CHECK(pmm_free_count() == 2 + 79 - meta(0x60000));
  CHECK(pmm_free(0x1000) == -EINVAL); /* rounded off the front */
  CHECK(pmm_free(0x5000) == -EINVAL);
}

static void test_init_errors(void) {
  CHECK(pmm_init(NULL, 0, 0, HHDM) == -ENOMEM);

  struct pmm_region tiny[] = {{0x5100, 0x200}};
  CHECK(pmm_init(tiny, 1, 0, HHDM) == -ENOMEM); /* no whole page */

  /* top at 2 MiB needs 5 pages of metadata: 4 pages aren't enough... */
  struct pmm_region four[] = {{0x1fc000, 0x4000}};
  CHECK(pmm_init(four, 1, 0, HHDM) == -ENOMEM);
  /* ...and 5 pages are used up by it */
  struct pmm_region five[] = {{0x1fb000, 0x5000}};
  CHECK(pmm_init(five, 1, 0, HHDM) == 0);
  CHECK(pmm_free_count() == 0);
  CHECK(pmm_alloc() == 0);

  static struct pmm_region many[PMM_MAX_REGIONS + 1];
  CHECK(pmm_init(many, PMM_MAX_REGIONS + 1, 0, HHDM) == -EINVAL);
}

/* `top` reserves bitmap space for memory added later, above all usable */
static void test_top(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}};
  CHECK(pmm_init(r, 1, 0, HHDM) == 0);
  struct pmm_region above = {0x100000, 0x10000};
  CHECK(pmm_add_region(&above) == -EINVAL); /* metadata ends at 0x9f000 */

  CHECK(pmm_init(r, 1, 0x110000, HHDM) == 0);
  size_t total = pmm_free_count();
  CHECK(total == 158 - meta(0x110000));     /* top adds no free frames */
  CHECK(pmm_add_region(&above) == 0);
  CHECK(pmm_free_count() == total + 16);
  struct pmm_region beyond = {0x110000, 0x1000};
  CHECK(pmm_add_region(&beyond) == -EINVAL);

  /* a top below the usable memory doesn't shrink the metadata */
  CHECK(pmm_init(r, 1, 0x2000, HHDM) == 0);
  CHECK(pmm_free_count() == 158 - meta(0x9f000));
}

static void test_add_region(void) {
  /* reclaimable memory in the hole between two usable regions */
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);
  size_t total = pmm_free_count();

  CHECK(pmm_free(0xa0000) == -EINVAL); /* not ours yet */
  struct pmm_region extra = {0xa0800, 0x10000}; /* -> [0xa1000, 0xb0000) */
  CHECK(pmm_add_region(&extra) == 0);
  CHECK(pmm_free_count() == total + 15);

  /* the new frames are allocatable and freeable like any other */
  uint64_t a = pmm_alloc();
  CHECK(pmm_free(a) == 0);
  CHECK(pmm_free(0xa1000) == -EINVAL); /* free already: double free */

  /* adding memory we already own changes nothing, even allocated frames */
  uint64_t held = pmm_alloc();
  struct pmm_region dup = {0x100000, 0x100000};
  CHECK(pmm_add_region(&dup) == 0);
  CHECK(pmm_free_count() == total + 15 - 1);
  CHECK(pmm_free(held) == 0);

  /* overlapping frame 0 never frees it */
  struct pmm_region low = {0x0, 0x1000};
  CHECK(pmm_add_region(&low) == 0);
  CHECK(pmm_free_count() == total + 15);

  /* past the top the metadata was sized for */
  struct pmm_region high = {MEM_SIZE, 0x1000};
  CHECK(pmm_add_region(&high) == -EINVAL);
  struct pmm_region tiny = {0xb0100, 0x100};
  CHECK(pmm_add_region(&tiny) == 0); /* no whole page: no-op */
  CHECK(pmm_free_count() == total + 15);
}

static void test_struct_page(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);

  uint64_t a = pmm_alloc();
  struct page *pg = phys_to_page(a);
  CHECK(pg != NULL);
  CHECK(page_to_phys(pg) == a);
  CHECK(pg->flags == 0 && pg->count == 0 && pg->owner == NULL);

  /* every byte of the frame maps to the same struct page */
  uint8_t *v = phys_to_virt(a);
  CHECK(virt_to_page(v) == pg);
  CHECK(virt_to_page(v + PAGE_SIZE - 1) == pg);
  CHECK(virt_to_page(v + PAGE_SIZE) == phys_to_page(a + PAGE_SIZE));

  /* outside the HHDM range of managed memory */
  CHECK(virt_to_page(mem - 1) == NULL);
  CHECK(virt_to_page(mem + MEM_SIZE) == NULL);
  CHECK(phys_to_page(MEM_SIZE) == NULL);

  /* metadata is reset when the frame changes hands */
  pg->flags = PG_SLAB;
  pg->count = 7;
  pg->owner = pg;
  CHECK(pmm_free(a) == 0);
  CHECK(pg->flags == 0 && pg->count == 0 && pg->owner == NULL);
  pg->flags = PG_SLAB; /* scribble on a free frame's metadata */
  CHECK(pmm_alloc() == a); /* next-fit hint: same frame comes back */
  CHECK(pg->flags == 0);
}

static void test_alloc_pages(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);
  size_t total = pmm_free_count();

  uint64_t run = pmm_alloc_pages(8);
  CHECK(run != 0 && run % PAGE_SIZE == 0);
  CHECK(pmm_free_count() == total - 8);
  for (int i = 0; i < 8; i++) /* each frame is really ours */
    CHECK(pmm_free(run + (uint64_t)i * PAGE_SIZE) == 0);
  CHECK(pmm_free_count() == total);

  run = pmm_alloc_pages(8);
  memset(phys_to_virt(run), 0xAB, 8 * PAGE_SIZE); /* contiguous in HHDM */
  CHECK(pmm_free_pages(run, 8) == 0);
  CHECK(pmm_free_count() == total);
  CHECK(pmm_free_pages(run, 8) == -EINVAL); /* double free */

  /* all-or-nothing: one bad frame in the run frees nothing */
  run = pmm_alloc_pages(4);
  CHECK(pmm_free(run + 3 * PAGE_SIZE) == 0);
  size_t before = pmm_free_count();
  CHECK(pmm_free_pages(run, 4) == -EINVAL);
  CHECK(pmm_free_count() == before);
  CHECK(pmm_free_pages(run, 3) == 0);

  /* a run can't span the hole between regions: the longest free run is
   * the high region minus its metadata, 256 - 5 = 251 frames */
  CHECK(pmm_alloc_pages(252) == 0);
  uint64_t big = pmm_alloc_pages(251);
  CHECK(big == 0x105000);
  CHECK(pmm_free_pages(big, 251) == 0);
  CHECK(pmm_alloc_pages(300) == 0);
  CHECK(pmm_alloc_pages(0) == 0);
  CHECK(pmm_free_count() == total);

  /* fragmentation: every other frame taken, so no run of 2 exists */
  static uint64_t frames[NFRAMES];
  size_t n = drain(frames);
  for (size_t i = 0; i < n; i++)
    if ((frames[i] / PAGE_SIZE) % 2 == 0)
      pmm_free(frames[i]);
  CHECK(pmm_free_count() > 0);
  CHECK(pmm_alloc_pages(2) == 0);
}

/* Random alloc/free against a model of which frames we hold. */
static void test_random(void) {
  struct pmm_region r[] = {{0x1000, 0x9e000}, {0x100000, 0x100000}};
  CHECK(pmm_init(r, 2, 0, HHDM) == 0);
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
  test_top();
  test_add_region();
  test_struct_page();
  test_alloc_pages();
  test_random();
  free(mem);
  return CHECK_DONE();
}
