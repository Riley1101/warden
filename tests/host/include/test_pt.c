#include "check.h"
#include "pmm.h"
#include "pt.h"

#include <abi/errno.h>
#include <stdlib.h>

/* Fake physical memory, as in test_pmm.c: phys_to_virt(p) == mem + p. */
#define MEM_SIZE (4UL * 1024 * 1024)
static uint8_t *mem;

static void reset(void) {
  memset(mem, 0x5A, MEM_SIZE); /* garbage: pt_new must zero its tables */
  struct pmm_region r = {0x1000, MEM_SIZE - 0x1000};
  CHECK(pmm_init(&r, 1, 0, (uint64_t)(uintptr_t)mem) == 0);
}

#define KBASE 0xffffffff80000000UL

static void test_map_lookup_unmap(void) {
  reset();
  uint64_t pml4 = pt_new();
  CHECK(pml4 != 0);
  uint64_t frame = pmm_alloc();

  CHECK(pt_lookup(pml4, KBASE) == 0);
  CHECK(pt_map(pml4, KBASE, frame, PTE_W | PTE_NX) == 0);

  uint64_t e = pt_lookup(pml4, KBASE);
  CHECK((e & PTE_ADDR) == frame);
  CHECK(e & PTE_P); /* implied */
  CHECK(e & PTE_W);
  CHECK(e & PTE_NX);
  CHECK(!(e & PTE_U));

  CHECK(pt_lookup(pml4, KBASE + PAGE_SIZE) == 0); /* neighbour untouched */

  CHECK(pt_unmap(pml4, KBASE) == frame);
  CHECK(pt_lookup(pml4, KBASE) == 0);
  CHECK(pt_unmap(pml4, KBASE) == 0); /* already gone */
}

static void test_errors(void) {
  reset();
  uint64_t pml4 = pt_new();
  uint64_t frame = pmm_alloc();

  CHECK(pt_map(pml4, KBASE + 1, frame, 0) == -EINVAL);   /* unaligned virt */
  CHECK(pt_map(pml4, KBASE, frame + 8, 0) == -EINVAL);   /* unaligned phys */
  CHECK(pt_map(pml4, 0x0000800000000000UL, frame, 0) == -EINVAL); /* hole */
  CHECK(pt_map(pml4, 0xffff7ffffffff000UL, frame, 0) == -EINVAL); /* hole */
  CHECK(pt_map(pml4, KBASE, 1UL << 52, 0) == -EINVAL);   /* phys too big */

  CHECK(pt_map(pml4, KBASE, frame, 0) == 0);
  CHECK(pt_map(pml4, KBASE, frame, PTE_W) == -EEXIST);
  CHECK(!(pt_lookup(pml4, KBASE) & PTE_W)); /* -EEXIST changed nothing */

  CHECK(pt_unmap(pml4, 0x0000800000000000UL) == 0);
  CHECK(pt_lookup(pml4, 0x0000800000000000UL) == 0);
}

/* Intermediate tables are allocated only when needed, and carry P|W|U. */
static void test_tables(void) {
  reset();
  uint64_t pml4 = pt_new();
  uint64_t frame = pmm_alloc();
  size_t before = pmm_free_count();

  CHECK(pt_map(pml4, KBASE, frame, 0) == 0);
  CHECK(pmm_free_count() == before - 3); /* PDPT + PD + PT */

  CHECK(pt_map(pml4, KBASE + PAGE_SIZE, frame, 0) == 0);
  CHECK(pmm_free_count() == before - 3); /* same PT */

  CHECK(pt_map(pml4, KBASE + (2UL << 20), frame, 0) == 0);
  CHECK(pmm_free_count() == before - 4); /* next 2 MiB: new PT */

  CHECK(pt_map(pml4, KBASE - (1UL << 30), frame, 0) == 0);
  CHECK(pmm_free_count() == before - 6); /* other 1 GiB: new PD + PT */

  uint64_t *top = phys_to_virt(pml4);
  uint64_t e = top[511]; /* KBASE lives in the last PML4 slot */
  CHECK((e & (PTE_P | PTE_W | PTE_U)) == (PTE_P | PTE_W | PTE_U));
  for (int i = 0; i < 511; i++)
    CHECK(top[i] == 0);

  /* lower half works too */
  CHECK(pt_map(pml4, 0x400000, frame, PTE_U) == 0);
  CHECK(top[0] & PTE_P);
}

static void test_oom(void) {
  reset();
  uint64_t pml4 = pt_new();
  uint64_t frame = pmm_alloc();
  while (pmm_alloc() != 0)
    ;
  CHECK(pt_map(pml4, KBASE, frame, 0) == -ENOMEM);
  CHECK(pt_lookup(pml4, KBASE) == 0);
  CHECK(pt_new() == 0);
}

/* Random map/unmap against a model; also checks the HHDM-style accesses
 * all land inside the fake memory (ASan). */
#define SLOTS 2048
static void test_random(void) {
  reset();
  uint64_t pml4 = pt_new();
  static uint64_t model[SLOTS]; /* slot -> mapped frame, 0 = unmapped */
  memset(model, 0, sizeof model);

  /* spread slots over several PML4/PDPT/PD entries */
  uint64_t base[4] = {0x0, 0x7f0000000000UL, 0xffff800000000000UL, KBASE};

  srand(1);
  for (int i = 0; i < 20000; i++) {
    int s = rand() % SLOTS;
    uint64_t virt = base[s % 4] + (uint64_t)(s / 4) * (PAGE_SIZE * 37);
    if (model[s] == 0) {
      uint64_t frame = (uint64_t)(1 + rand() % 1000) * PAGE_SIZE;
      CHECK(pt_map(pml4, virt, frame, PTE_W) == 0);
      model[s] = frame;
    } else {
      CHECK(pt_unmap(pml4, virt) == model[s]);
      model[s] = 0;
    }
    int t = rand() % SLOTS;
    uint64_t tv = base[t % 4] + (uint64_t)(t / 4) * (PAGE_SIZE * 37);
    CHECK((pt_lookup(pml4, tv) & PTE_ADDR) == model[t]);
  }
}

/* pt_free_user returns every user frame and table, and leaves the kernel
 * half's tables alone (other address spaces share them). */
static void test_free_user(void) {
  reset();
  size_t before = pmm_free_count();
  uint64_t pml4 = pt_new();
  uint64_t kframe = pmm_alloc();
  CHECK(pt_map(pml4, KBASE, kframe, PTE_W) == 0); /* +3 kernel tables */
  size_t kernel_used = before - pmm_free_count();

  const uint64_t user[] = {0x400000, 0x401000, 0x7ff000, 0x7fffffffe000UL};
  for (int i = 0; i < 4; i++)
    CHECK(pt_map(pml4, user[i], pmm_alloc(), PTE_W | PTE_U) == 0);
  CHECK(pmm_free_count() < before - kernel_used);

  pt_free_user(pml4);
  /* back to just the kernel frame and its tables, minus the PML4 */
  CHECK(pmm_free_count() == before - kernel_used + 1);
  CHECK(pmm_free(kframe) == 0); /* still ours: not freed by pt_free_user */
}

/* pt_copy_user: same pages, same flags, fresh frames with the same bytes;
 * the kernel half is not copied. */
static void test_copy_user(void) {
  reset();
  uint64_t src = pt_new();
  const uint64_t user[] = {0x400000, 0x401000, 0x7fffffffe000UL};
  const uint64_t flags[] = {PTE_U, PTE_U | PTE_W | PTE_NX, PTE_U | PTE_W};
  for (int i = 0; i < 3; i++) {
    uint64_t f = pmm_alloc();
    memset(phys_to_virt(f), 'a' + i, PAGE_SIZE);
    CHECK(pt_map(src, user[i], f, flags[i]) == 0);
  }
  CHECK(pt_map(src, KBASE, pmm_alloc(), PTE_W) == 0);

  uint64_t dst = pt_new();
  CHECK(pt_copy_user(dst, src) == 0);
  for (int i = 0; i < 3; i++) {
    uint64_t s = pt_lookup(src, user[i]), d = pt_lookup(dst, user[i]);
    CHECK(d != 0);
    CHECK((d & PTE_ADDR) != (s & PTE_ADDR));
    CHECK((d & ~PTE_ADDR) == (s & ~PTE_ADDR));
    const uint8_t *p = phys_to_virt(d & PTE_ADDR);
    CHECK(p[0] == 'a' + i && p[PAGE_SIZE - 1] == 'a' + i);
  }
  CHECK(pt_lookup(dst, KBASE) == 0);

  /* writing the copy leaves the original alone */
  *(uint8_t *)phys_to_virt(pt_lookup(dst, user[1]) & PTE_ADDR) = 'z';
  CHECK(*(uint8_t *)phys_to_virt(pt_lookup(src, user[1]) & PTE_ADDR) == 'b');

  /* Out of memory partway (a copy needs 3 tables + 3 frames): reported,
   * and pt_free_user gives back everything the partial copy took. */
  uint64_t part = pt_new();
  while (pmm_free_count() > 5)
    (void)pmm_alloc();
  CHECK(pt_copy_user(part, src) == -ENOMEM);
  pt_free_user(part);
  CHECK(pmm_free_count() == 5 + 1); /* + the PML4 itself */
}

int main(void) {
  mem = malloc(MEM_SIZE);
  test_map_lookup_unmap();
  test_errors();
  test_tables();
  test_oom();
  test_random();
  test_free_user();
  test_copy_user();
  free(mem);
  return CHECK_DONE();
}
