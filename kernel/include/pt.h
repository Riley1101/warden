#pragma once

#include <stdint.h>

/*
 * x86_64 4-level page tables (PML4 -> PDPT -> PD -> PT), 4 KiB pages only.
 *
 * Must stay free of hardware code: it is compiled into the kernel AND into
 * host unit tests with sanitizers. Tables are physical frames from the PMM,
 * reached through phys_to_virt(). Nothing here touches CR3 or the TLB; the
 * caller (vmm.c) flushes after changing a live address space.
 */

#define PTE_P   (1UL << 0)  /* present */
#define PTE_W   (1UL << 1)  /* writable */
#define PTE_U   (1UL << 2)  /* user accessible */
#define PTE_PWT (1UL << 3)  /* write-through */
#define PTE_PCD (1UL << 4)  /* cache disable */
#define PTE_A   (1UL << 5)  /* accessed (set by CPU) */
#define PTE_D   (1UL << 6)  /* dirty (set by CPU) */
#define PTE_PS  (1UL << 7)  /* huge page (in PDPT/PD entries) */
#define PTE_G   (1UL << 8)  /* global: survives CR3 reloads */
#define PTE_NX  (1UL << 63) /* no execute (needs EFER.NXE) */

#define PTE_ADDR 0x000FFFFFFFFFF000UL /* physical address bits 12..51 */

/* Allocate a zeroed table. Returns its physical address, or 0 if OOM. */
uint64_t pt_new(void);

/*
 * Map the 4 KiB page at `virt` to frame `phys` with `flags` (PTE_P is
 * implied). Missing intermediate tables are allocated and zeroed; they get
 * P|W|U so that the leaf entry alone decides the permissions.
 *
 * Returns 0, -EINVAL if `virt`/`phys` is unaligned or `virt` non-canonical,
 * -EEXIST if `virt` is already mapped, or -ENOMEM if a table can't be
 * allocated (tables allocated before the failure stay in place).
 */
int pt_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/*
 * Remove the mapping for `virt`. Returns the frame it pointed at, or 0 if
 * nothing was mapped. Does not free the frame or empty tables.
 */
uint64_t pt_unmap(uint64_t pml4, uint64_t virt);

/* The leaf entry for `virt` (address | flags), or 0 if not mapped. */
uint64_t pt_lookup(uint64_t pml4, uint64_t virt);

/*
 * Free the lower half of `pml4` (PML4 slots 0..255): every frame mapped
 * there, every table, and then the PML4 itself. The higher half is shared
 * kernel tables and is left alone. Assumes the address space owns every
 * frame it maps (no sharing yet). Must not be the active CR3.
 */
void pt_free_user(uint64_t pml4);
