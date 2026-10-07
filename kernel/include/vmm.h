#pragma once

#include <pmm.h>
#include <pt.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Build the kernel address space and switch CR3 to it:
 *   - the kernel image at its link address, with per-section permissions
 *     (text R-X, rodata R--, data/bss RW-; W^X throughout)
 *
 *   - the HHDM: each range in `hhdm` mapped at phys_to_virt(phys), RW + NX
 *
 *   - PML4 slots 256..511 (the higher half) pre-allocated, so every future
 *     address space can share the kernel by copying those 256 entries
 *
 * Also enables EFER.NXE and CR0.WP. Panics on failure.
 *
 * `kernel_phys`/`kernel_virt`: where the bootloader loaded the image.
 */
void vmm_init(const struct pmm_region *hhdm, size_t n, uint64_t kernel_phys,
              uint64_t kernel_virt);

/* Map / unmap one page in the kernel address space. Same errors as pt_map.
 * vmm_unmap flushes the TLB entry and returns the old frame (or 0). */
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

uint64_t vmm_unmap(uint64_t virt);

/* Physical address of the kernel PML4. */
uint64_t vmm_kernel_pml4(void);
