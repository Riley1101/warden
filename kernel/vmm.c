#include "vmm.h"

#include <abi/errno.h>
#include <cpu.h>
#include <kprintf.h>
#include <kstring.h>
#include <lock.h>

/* linker.ld */
extern char __kernel_start[], __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[], __data_start[], __data_end[];

static uint64_t kernel_pml4;
static uint64_t kphys, kvirt; /* load addresses of the kernel image */

/* Guards the kernel page tables after vmm_init. */
static struct lock vmm_lock = LOCK_INIT("vmm");

static uint64_t align_up(uint64_t x) { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }
static uint64_t align_down(uint64_t x) { return x & ~(PAGE_SIZE - 1); }

static void enable_nx_wp(void) {
  uint32_t a, b, c, d;
  cpuid(0x80000001, &a, &b, &c, &d);
  if (!(d & (1u << 20)))
    panic("vmm: CPU has no NX bit");
  wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
  write_cr0(read_cr0() | CR0_WP);
}

/* Map one section of the kernel image: its virtual range, backed by the
 * same offset into the physical image. */
static void map_kernel_section(const char *start, const char *end,
                               uint64_t flags) {
  uint64_t s = align_down((uint64_t)start);
  uint64_t e = align_up((uint64_t)end);
  for (uint64_t v = s; v < e; v += PAGE_SIZE) {
    int err = pt_map(kernel_pml4, v, v - kvirt + kphys, flags | PTE_G);
    if (err != 0)
      panic("vmm: kernel map %lx failed: %d", v, err);
  }
}

/* Ranges are rounded outward to pages, so neighbours may share a page:
 * that page is already mapped (to the same frame), which is fine. */
static void map_hhdm_range(const struct pmm_region *r) {
  uint64_t s = align_down(r->base);
  uint64_t e = align_up(r->base + r->len);
  for (uint64_t p = s; p < e; p += PAGE_SIZE) {
    int err = pt_map(kernel_pml4, (uint64_t)phys_to_virt(p), p,
                     PTE_W | PTE_NX | PTE_G);
    if (err != 0 && err != -EEXIST)
      panic("vmm: HHDM map %lx failed: %d", p, err);
  }
}

void vmm_init(const struct pmm_region *hhdm, size_t n, uint64_t kernel_phys,
              uint64_t kernel_virt) {
  kphys = kernel_phys;
  kvirt = kernel_virt;
  enable_nx_wp();

  kernel_pml4 = pt_new();
  if (kernel_pml4 == 0)
    panic("vmm: no memory for the PML4");

  /* Higher half: allocate every PML4 entry now. Later address spaces copy
   * these pointers, so anything mapped here later is shared by all. */
  uint64_t *top = phys_to_virt(kernel_pml4);
  for (int i = 256; i < 512; i++) {
    uint64_t t = pt_new();
    if (t == 0)
      panic("vmm: no memory for the kernel half");
    top[i] = t | PTE_P | PTE_W;
  }

  /* .limine_requests: Limine wrote responses here; we only read them */
  map_kernel_section(__kernel_start, __text_start, PTE_W | PTE_NX);
  map_kernel_section(__text_start, __text_end, 0);
  map_kernel_section(__rodata_start, __rodata_end, PTE_NX);
  map_kernel_section(__data_start, __data_end, PTE_W | PTE_NX);

  for (size_t i = 0; i < n; i++)
    map_hhdm_range(&hhdm[i]);

  write_cr3(kernel_pml4);
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
  /* A not-present entry is never cached in the TLB: no flush needed. */
  lock_acquire(&vmm_lock);
  int err = pt_map(kernel_pml4, virt, phys, flags | PTE_G);
  lock_release(&vmm_lock);
  return err;
}

uint64_t vmm_unmap(uint64_t virt) {
  lock_acquire(&vmm_lock);
  uint64_t phys = pt_unmap(kernel_pml4, virt);
  if (phys != 0)
    invlpg(virt);
  lock_release(&vmm_lock);
  return phys;
}

uint64_t vmm_kernel_pml4(void) { return kernel_pml4; }

uint64_t vmm_space_new(void) {
  uint64_t pml4 = pt_new();
  if (pml4 == 0)
    return 0;
  uint64_t *top = phys_to_virt(pml4);
  const uint64_t *kernel = phys_to_virt(kernel_pml4);
  for (int i = 256; i < 512; i++)
    top[i] = kernel[i];
  return pml4;
}

int vmm_space_map(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
  if (virt >= USER_TOP)
    return -EINVAL; /* a user page in the shared kernel half: everyone's */
  return pt_map(pml4, virt, phys, flags | PTE_U);
}

int vmm_space_write(uint64_t pml4, uint64_t virt, const void *src, size_t n) {
  const uint8_t *s = src;
  while (n > 0) {
    uint64_t page = align_down(virt);
    uint64_t e = virt < USER_TOP ? pt_lookup(pml4, page) : 0;
    if (!(e & PTE_U))
      return -EFAULT;
    size_t off = virt - page;
    size_t chunk = PAGE_SIZE - off < n ? PAGE_SIZE - off : n;
    memcpy((uint8_t *)phys_to_virt(e & PTE_ADDR) + off, s, chunk);
    virt += chunk;
    s += chunk;
    n -= chunk;
  }
  return 0;
}

int vmm_space_copy(uint64_t dst, uint64_t src) { return pt_copy_user(dst, src); }

void vmm_space_free(uint64_t pml4) {
  if ((read_cr3() & PTE_ADDR) == pml4)
    panic("vmm: freeing the active address space");
  pt_free_user(pml4);
}

void vmm_switch(uint64_t pml4) {
  if ((read_cr3() & PTE_ADDR) != pml4)
    write_cr3(pml4);
}
