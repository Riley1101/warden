#include "pmm.h"

#include <abi/errno.h>
#include <lock.h>

#define BITS 64 /* frames per bitmap word */

static struct pmm_region regions[PMM_MAX_REGIONS]; /* page-aligned copies */
static size_t nregions;

static uint64_t hhdm_base;
static uint64_t *bitmap;    /* bit set = frame used */
static size_t nframes;      /* frames covered by the metadata: [0, top) */
static size_t nwords;
static struct page *pages; /* one per frame in [0, top) */
static uint64_t meta_phys; /* bitmap, then the struct page array */
static size_t meta_pages;
static size_t nfree;
static size_t hint; /* word to start the next search at (next-fit) */

/* Guards everything above after pmm_init. The public entry points take it;
 * the static helpers below assume it is held. */
static struct lock pmm_lock = LOCK_INIT("pmm");

static uint64_t align_up(uint64_t x) { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }
static uint64_t align_down(uint64_t x) { return x & ~(PAGE_SIZE - 1); }

static int test_bit(size_t f) { return (bitmap[f / BITS] >> (f % BITS)) & 1; }
static void set_bit(size_t f) { bitmap[f / BITS] |= 1UL << (f % BITS); }
static void clear_bit(size_t f) { bitmap[f / BITS] &= ~(1UL << (f % BITS)); }

void *phys_to_virt(uint64_t phys) { return (void *)(uintptr_t)(hhdm_base + phys); }

int pmm_init(const struct pmm_region *in, size_t n, uint64_t top_hint,
             uint64_t hhdm) {
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

  if (align_up(top_hint) > top)
    top = align_up(top_hint);

  hhdm_base = hhdm;
  nframes = top / PAGE_SIZE;
  nwords = (nframes + BITS - 1) / BITS;
  size_t bitmap_pages = align_up(nwords * sizeof(uint64_t)) / PAGE_SIZE;
  size_t array_pages = align_up(nframes * sizeof(struct page)) / PAGE_SIZE;
  meta_pages = bitmap_pages + array_pages;
  if (regions[largest].len < meta_pages * PAGE_SIZE)
    return -ENOMEM;
  meta_phys = regions[largest].base;
  bitmap = phys_to_virt(meta_phys);
  pages = phys_to_virt(meta_phys + bitmap_pages * PAGE_SIZE);
  for (size_t f = 0; f < nframes; f++)
    pages[f] = (struct page){0};

  /* Everything starts used (holes, reserved memory, tail bits of the last
   * word); then free the usable frames, then take back the metadata's. */
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
  for (size_t p = 0; p < meta_pages; p++) {
    set_bit(meta_phys / PAGE_SIZE + p);
    nfree--;
  }
  if (!test_bit(0)) { /* 0 is the "out of memory" value */
    set_bit(0);
    nfree--;
  }

  hint = 0;
  return 0;
}

static uint64_t alloc_frame(void) {
  if (nfree == 0)
    return 0;
  for (size_t i = 0; i < nwords; i++) {
    size_t w = (hint + i) % nwords;
    if (bitmap[w] == ~0UL)
      continue; /* all 64 frames used: skip the word in one compare */
    size_t f = w * BITS + (size_t)__builtin_ctzl(~bitmap[w]);
    set_bit(f);
    pages[f] = (struct page){0};
    nfree--;
    hint = w;
    return (uint64_t)f * PAGE_SIZE;
  }
  return 0; /* unreachable while nfree is right */
}

static int in_meta(size_t f) {
  size_t m = meta_phys / PAGE_SIZE;
  return f >= m && f < m + meta_pages;
}

/* Is frame `f` one that pmm_alloc could ever hand out? */
static int managed(size_t f) {
  if (f == 0 || in_meta(f))
    return 0;
  for (size_t i = 0; i < nregions; i++) {
    size_t first = regions[i].base / PAGE_SIZE;
    if (f >= first && f < first + regions[i].len / PAGE_SIZE)
      return 1;
  }
  return 0;
}

/* Would pmm_free(frame f) succeed? */
static int freeable(size_t f) {
  return managed(f) && test_bit(f); /* bit clear: double free */
}

static int free_frame(uint64_t phys) {
  if (phys % PAGE_SIZE != 0 || !freeable(phys / PAGE_SIZE))
    return -EINVAL;
  size_t f = phys / PAGE_SIZE;
  clear_bit(f);
  pages[f] = (struct page){0};
  nfree++;
  return 0;
}

static uint64_t alloc_frames(size_t n) {
  if (n == 0 || n > nfree)
    return 0;
  if (n == 1)
    return alloc_frame();
  size_t run = 0;
  for (size_t f = 1; f < nframes; f++) {
    run = test_bit(f) ? 0 : run + 1;
    if (run == n) {
      size_t first = f + 1 - n;
      for (size_t i = first; i <= f; i++) {
        set_bit(i);
        pages[i] = (struct page){0};
      }
      nfree -= n;
      return (uint64_t)first * PAGE_SIZE;
    }
  }
  return 0;
}

static int free_frames(uint64_t phys, size_t n) {
  if (phys % PAGE_SIZE != 0 || n == 0)
    return -EINVAL;
  size_t first = phys / PAGE_SIZE;
  for (size_t i = 0; i < n; i++)
    if (!freeable(first + i))
      return -EINVAL;
  for (size_t i = 0; i < n; i++)
    free_frame((first + i) * PAGE_SIZE);
  return 0;
}

struct page *phys_to_page(uint64_t phys) {
  size_t f = phys / PAGE_SIZE;
  return f < nframes ? &pages[f] : NULL;
}

struct page *virt_to_page(const void *p) {
  uint64_t v = (uint64_t)(uintptr_t)p;
  if (v < hhdm_base)
    return NULL;
  return phys_to_page(v - hhdm_base);
}

uint64_t page_to_phys(const struct page *pg) {
  return (uint64_t)(pg - pages) * PAGE_SIZE;
}

static int add_region(const struct pmm_region *r) {
  uint64_t start = align_up(r->base);
  uint64_t end = align_down(r->base + r->len);
  if (end <= start)
    return 0;
  if (end / PAGE_SIZE > nframes)
    return -EINVAL;
  if (nregions == PMM_MAX_REGIONS)
    return -ENOMEM;

  /* Free before recording the region, so managed() still says "no" for
   * frames that are new to us, and "yes" for ones we already own. */
  for (size_t f = start / PAGE_SIZE; f < end / PAGE_SIZE; f++) {
    if (f == 0 || in_meta(f) || managed(f))
      continue; /* never hand out frame 0 or the metadata; don't double-free */
    clear_bit(f);
    nfree++;
  }
  regions[nregions++] = (struct pmm_region){start, end - start};
  return 0;
}

size_t pmm_free_count(void) { return nfree; }

/* ---- locked entry points -------------------------------------------------- */

uint64_t pmm_alloc(void) {
  lock_acquire(&pmm_lock);
  uint64_t phys = alloc_frame();
  lock_release(&pmm_lock);
  return phys;
}

int pmm_free(uint64_t phys) {
  lock_acquire(&pmm_lock);
  int err = free_frame(phys);
  lock_release(&pmm_lock);
  return err;
}

uint64_t pmm_alloc_pages(size_t n) {
  lock_acquire(&pmm_lock);
  uint64_t phys = alloc_frames(n);
  lock_release(&pmm_lock);
  return phys;
}

int pmm_free_pages(uint64_t phys, size_t n) {
  lock_acquire(&pmm_lock);
  int err = free_frames(phys, n);
  lock_release(&pmm_lock);
  return err;
}

int pmm_add_region(const struct pmm_region *r) {
  lock_acquire(&pmm_lock);
  int err = add_region(r);
  lock_release(&pmm_lock);
  return err;
}
