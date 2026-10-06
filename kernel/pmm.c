#include "pmm.h"

#include <abi/errno.h>

#define BITS 64 /* frames per bitmap word */

static struct pmm_region regions[PMM_MAX_REGIONS]; /* page-aligned copies */
static size_t nregions;

static uint64_t hhdm_base;
static uint64_t *bitmap;    /* bit set = frame used */
static size_t nframes;      /* frames covered by the bitmap: [0, top) */
static size_t nwords;
static uint64_t bitmap_phys;
static size_t bitmap_pages;
static size_t nfree;
static size_t hint; /* word to start the next search at (next-fit) */

static uint64_t align_up(uint64_t x) { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }
static uint64_t align_down(uint64_t x) { return x & ~(PAGE_SIZE - 1); }

static int test_bit(size_t f) { return (bitmap[f / BITS] >> (f % BITS)) & 1; }
static void set_bit(size_t f) { bitmap[f / BITS] |= 1UL << (f % BITS); }
static void clear_bit(size_t f) { bitmap[f / BITS] &= ~(1UL << (f % BITS)); }

void *phys_to_virt(uint64_t phys) { return (void *)(uintptr_t)(hhdm_base + phys); }

int pmm_init(const struct pmm_region *in, size_t n, uint64_t hhdm) {
  if (n > PMM_MAX_REGIONS)
    return -EINVAL;

  /* Keep only whole pages: a partial page at either end is unusable. */
  nregions = 0;
  uint64_t top = 0;
  size_t largest = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t start = align_up(in[i].base);
    uint64_t end = align_down(in[i].base + in[i].len);
    if (end <= start)
      continue;
    regions[nregions] = (struct pmm_region){start, end - start};
    if (regions[nregions].len > regions[largest].len)
      largest = nregions;
    if (end > top)
      top = end;
    nregions++;
  }
  if (nregions == 0)
    return -ENOMEM;

  hhdm_base = hhdm;
  nframes = top / PAGE_SIZE;
  nwords = (nframes + BITS - 1) / BITS;
  bitmap_pages = align_up(nwords * sizeof(uint64_t)) / PAGE_SIZE;
  if (regions[largest].len < bitmap_pages * PAGE_SIZE)
    return -ENOMEM;
  bitmap_phys = regions[largest].base;
  bitmap = phys_to_virt(bitmap_phys);

  /* Everything starts used (holes, reserved memory, tail bits of the last
   * word); then free the usable frames, then take back the bitmap's own. */
  for (size_t w = 0; w < nwords; w++)
    bitmap[w] = ~0UL;
  nfree = 0;
  for (size_t i = 0; i < nregions; i++) {
    size_t first = regions[i].base / PAGE_SIZE;
    size_t last = first + regions[i].len / PAGE_SIZE;
    for (size_t f = first; f < last; f++) {
      if (test_bit(f)) { /* guards against overlapping regions */
        clear_bit(f);
        nfree++;
      }
    }
  }
  for (size_t p = 0; p < bitmap_pages; p++) {
    set_bit(bitmap_phys / PAGE_SIZE + p);
    nfree--;
  }
  if (!test_bit(0)) { /* 0 is the "out of memory" value */
    set_bit(0);
    nfree--;
  }

  hint = 0;
  return 0;
}

uint64_t pmm_alloc(void) {
  if (nfree == 0)
    return 0;
  for (size_t i = 0; i < nwords; i++) {
    size_t w = (hint + i) % nwords;
    if (bitmap[w] == ~0UL)
      continue; /* all 64 frames used: skip the word in one compare */
    size_t f = w * BITS + (size_t)__builtin_ctzl(~bitmap[w]);
    set_bit(f);
    nfree--;
    hint = w;
    return (uint64_t)f * PAGE_SIZE;
  }
  return 0; /* unreachable while nfree is right */
}

/* Is frame `f` one that pmm_alloc could ever hand out? */
static int managed(size_t f) {
  if (f == 0)
    return 0;
  size_t bm = bitmap_phys / PAGE_SIZE;
  if (f >= bm && f < bm + bitmap_pages)
    return 0;
  for (size_t i = 0; i < nregions; i++) {
    size_t first = regions[i].base / PAGE_SIZE;
    if (f >= first && f < first + regions[i].len / PAGE_SIZE)
      return 1;
  }
  return 0;
}

int pmm_free(uint64_t phys) {
  if (phys % PAGE_SIZE != 0)
    return -EINVAL;
  size_t f = phys / PAGE_SIZE;
  if (!managed(f))
    return -EINVAL;
  if (!test_bit(f))
    return -EINVAL; /* double free */
  clear_bit(f);
  nfree++;
  return 0;
}

size_t pmm_free_count(void) { return nfree; }
