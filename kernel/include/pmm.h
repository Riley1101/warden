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
 * `hhdm` is the virtual address where physical address 0 is mapped.
 * The bitmap is carved out of the largest region. Frame 0 is never handed
 * out, so 0 can mean "no memory".
 *
 * Returns 0, -EINVAL if there are more than PMM_MAX_REGIONS regions,
 * or -ENOMEM if there is no usable page or no room for the bitmap.
 */
int pmm_init(const struct pmm_region *regions, size_t n, uint64_t hhdm);

/* Allocate one frame. Returns its physical address (not zeroed), or 0. */
uint64_t pmm_alloc(void);

/*
 * Free a frame from pmm_alloc. Returns 0, or -EINVAL if `phys` is unaligned,
 * not managed by the PMM (reserved, the bitmap itself, frame 0), or already
 * free. A nonzero return is always a caller bug.
 */
int pmm_free(uint64_t phys);

/* Number of free frames. */
size_t pmm_free_count(void);

/* Where the kernel can read/write physical address `phys`. */
void *phys_to_virt(uint64_t phys);
