#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Physical memory manager: a bitmap of 4 KiB frames (1 = used, 0 = free).
 *
 * Must stay free of hardware code: it is compiled into the kernel AND into
 * host unit tests with sanitizers. All memory it touches is reached through
 * `hhdm + phys`, so a host test can point `hhdm` at a malloc'd buffer.
 */

#define PAGE_SIZE 4096UL

/* A range of usable physical RAM. Need not be page-aligned. */

struct pmm_region {
  uint64_t base;
  uint64_t len;
};

#define PMM_MAX_REGIONS 64

/*
 * Take ownership of the usable `regions` (copied; the array may go away).
 *
 * `top`: the bitmap covers physical memory [0, top), rounded up to a page,
 * so pmm_add_region can later add memory below it. 0 means "up to the end
 * of the highest usable region".
 *
 * `hhdm` is the virtual address where physical address 0 is mapped.
 * The bitmap and the struct page array are carved out of the largest
 * region. Frame 0 is never handed
 * out, so 0 can mean "no memory".
 *
 * Returns 0, -EINVAL if there are more than PMM_MAX_REGIONS regions,
 * or -ENOMEM if there is no usable page or no room for the metadata.
 */
int pmm_init(const struct pmm_region *regions, size_t n, uint64_t top,
             uint64_t hhdm);

/*
 * Hand more memory to the PMM after init, e.g. bootloader-reclaimable memory
 * once nothing uses it. Partial pages at either end are dropped, and frames
 * the PMM already manages are left alone.
 *
 * Returns 0, -EINVAL if the region reaches past the memory pmm_init sized
 * the bitmap for, or -ENOMEM if the region table is full.
 */
int pmm_add_region(const struct pmm_region *r);

/*
 * Per-frame metadata ("struct page"), one per physical frame, kept in an
 * array next to the bitmap -- out of band, so overflowing a heap object
 * can't corrupt the allocator's bookkeeping.
 *
 * Reset to all zeros whenever a frame is allocated or freed; the owner of
 * an allocated frame (the slab allocator, kmalloc) fills in what it needs.
 */
struct page {
  uint32_t flags;
  uint32_t count;     /* PG_SLAB: objects in use; PG_LARGE: pages in run */
  void *owner;        /* PG_SLAB: the struct kmem_cache it belongs to */
  void *freelist;     /* PG_SLAB: first free object, NULL when full */
  struct page *next;  /* PG_SLAB: next slab on the cache's partial list */
};

#define PG_SLAB  (1u << 0) /* a page of slab objects */
#define PG_LARGE (1u << 1) /* first page of a multi-page kmalloc */

/* Metadata for the frame at `phys`, or NULL if beyond managed memory. */
struct page *phys_to_page(uint64_t phys);

/* Metadata for the frame containing HHDM address `p`, or NULL if `p` is
 * not an HHDM address of managed memory. */
struct page *virt_to_page(const void *p);

/* Physical address of the frame `pg` describes. */
uint64_t page_to_phys(const struct page *pg);

/* Allocate one frame. Returns its physical address (not zeroed), or 0. */
uint64_t pmm_alloc(void);

/*
 * Free a frame from pmm_alloc. Returns 0, or -EINVAL if `phys` is unaligned,
 * not managed by the PMM (reserved, the bitmap itself, frame 0), or already
 * free. A nonzero return is always a caller bug.
 */
int pmm_free(uint64_t phys);

/*
 * Allocate `n` physically contiguous frames. Returns the first one's
 * address, or 0. Slower than pmm_alloc (a linear search for a free run);
 * meant for the rare large allocation.
 */
uint64_t pmm_alloc_pages(size_t n);

/* Free a run from pmm_alloc_pages. All-or-nothing: returns -EINVAL and
 * frees nothing if any frame in the run fails pmm_free's checks. */
int pmm_free_pages(uint64_t phys, size_t n);

/* Number of free frames. */
size_t pmm_free_count(void);

/* Where the kernel can read/write physical address `phys`. */
void *phys_to_virt(uint64_t phys);
