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

/* ---- user address spaces -------------------------------------------------- */

/* User addresses are the canonical lower half: [0, USER_TOP). */
#define USER_TOP 0x0000800000000000UL

/* A new address space: empty lower half, the kernel half shared (PML4 slots
 * 256..511 copied from the kernel's). Returns its PML4, or 0 if OOM. */
uint64_t vmm_space_new(void);

/*
 * Map one user page in `pml4`; PTE_U is added. -EINVAL if `virt` is not a
 * user address, otherwise as pt_map. No lock: an address space belongs to
 * one thread, and user mappings never touch the shared kernel tables.
 */
int vmm_space_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/* Free an address space and every frame it maps. Not while it is in CR3. */
void vmm_space_free(uint64_t pml4);

/* Load `pml4` into CR3, unless it is already there. */
void vmm_switch(uint64_t pml4);
