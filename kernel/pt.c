#include "pt.h"

#include <abi/errno.h>
#include <pmm.h>
#include <stddef.h>

#define ENTRIES 512

/* Table index for `virt` at `level` (3 = PML4, 0 = PT): 9 bits each. */
static unsigned index_of(uint64_t virt, int level) {
  return (virt >> (12 + 9 * level)) & (ENTRIES - 1);
}

/* Canonical: bits 63..47 are all copies of bit 47. */
static int canonical(uint64_t virt) {
  uint64_t top = virt >> 47;
  return top == 0 || top == 0x1FFFF;
}

static uint64_t *table(uint64_t phys) { return phys_to_virt(phys); }

uint64_t pt_new(void) {
  uint64_t phys = pmm_alloc();
  if (phys == 0)
    return 0;
  uint64_t *t = table(phys);
  for (int i = 0; i < ENTRIES; i++)
    t[i] = 0;
  return phys;
}

/*
 * Walk to the PT entry for `virt`. With `create`, missing tables are
 * allocated; without, a missing table returns NULL. Huge pages are never
 * created here, so finding one means someone else built this table.
 */
static uint64_t *walk(uint64_t pml4, uint64_t virt, int create) {
  uint64_t *t = table(pml4);
  for (int level = 3; level > 0; level--) {
    uint64_t *e = &t[index_of(virt, level)];
    if (!(*e & PTE_P)) {
      if (!create)
        return NULL;
      uint64_t next = pt_new();
      if (next == 0)
        return NULL;
      *e = next | PTE_P | PTE_W | PTE_U;
    } else if (*e & PTE_PS) {
      return NULL;
    }
    t = table(*e & PTE_ADDR);
  }
  return &t[index_of(virt, 0)];
}

int pt_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
  if (virt % PAGE_SIZE != 0 || phys % PAGE_SIZE != 0 || !canonical(virt))
    return -EINVAL;
  if (phys & ~PTE_ADDR)
    return -EINVAL; /* beyond 52-bit physical */
  if (pt_lookup(pml4, virt) != 0)
    return -EEXIST;
  uint64_t *e = walk(pml4, virt, 1);
  if (e == NULL)
    return -ENOMEM;
  *e = phys | (flags & ~PTE_ADDR) | PTE_P;
  return 0;
}

uint64_t pt_unmap(uint64_t pml4, uint64_t virt) {
  if (!canonical(virt))
    return 0;
  uint64_t *e = walk(pml4, virt, 0);
  if (e == NULL || !(*e & PTE_P))
    return 0;
  uint64_t phys = *e & PTE_ADDR;
  *e = 0;
  return phys;
}

uint64_t pt_lookup(uint64_t pml4, uint64_t virt) {
  if (!canonical(virt))
    return 0;
  uint64_t *e = walk(pml4, virt, 0);
  if (e == NULL || !(*e & PTE_P))
    return 0;
  return *e;
}
