#include "check.h"
#include "pmm.h"
#include "slab.h"

#include <stdlib.h>

/* Fake physical memory, as in test_pmm.c: phys_to_virt(p) == mem + p. */
#define MEM_SIZE (4UL * 1024 * 1024)
static uint8_t *mem;
static size_t pmm_start; /* free frames right after reset() */
static int bugs;
static const char *last_bug;

static void reset(void) {
  bugs = 0;
  last_bug = NULL;
  memset(mem, 0x5A, MEM_SIZE);
  struct pmm_region r = {0x1000, MEM_SIZE - 0x1000};
  CHECK(pmm_init(&r, 1, 0, (uint64_t)(uintptr_t)mem) == 0);
  kmalloc_init();
  pmm_start = pmm_free_count();
}

/* slab_bug: the kernel panics; here we record the last report. */
void slab_bug(const char *what, const void *p) {
  (void)p;
  bugs++;
  last_bug = what;
}

#define EXPECT_BUG(stmt, substr)                                               \
  do {                                                                         \
    int before_ = bugs;                                                        \
    stmt;                                                                      \
    CHECK(bugs == before_ + 1);                                                \
    CHECK(last_bug != NULL && strstr(last_bug, substr) != NULL);               \
  } while (0)

static void test_cache_basics(void) {
  reset();
  struct kmem_cache c;
  kmem_cache_init(&c, "test-24", 24);
  CHECK(c.size == 32); /* rounded up to a multiple of 16 */
  CHECK(c.per_slab == PAGE_SIZE / 32);

  /* fill one slab exactly, tagging every object */
  static uint8_t *objs[PAGE_SIZE / 32 + 1];
  for (uint32_t i = 0; i < c.per_slab; i++) {
    objs[i] = kmem_cache_alloc(&c);
    CHECK(objs[i] != NULL);
    CHECK((uintptr_t)objs[i] % 16 == 0);
    memset(objs[i], (int)i, c.size);
  }
  CHECK(c.nslabs == 1);
  CHECK(c.inuse == c.per_slab);
  CHECK(c.partial == NULL); /* full slabs aren't on the list */

  /* one more needs a second slab */
  objs[c.per_slab] = kmem_cache_alloc(&c);
  CHECK(c.nslabs == 2);
  CHECK(pmm_free_count() == pmm_start - 2);

  /* no object was overwritten by another */
  for (uint32_t i = 0; i < c.per_slab; i++)
    for (uint32_t b = 0; b < c.size; b++)
      CHECK(objs[i][b] == (uint8_t)i);

  /* metadata is in struct page, not in the page */
  struct page *pg = virt_to_page(objs[0]);
  CHECK(pg->flags == PG_SLAB && pg->owner == &c && pg->count == c.per_slab);

  /* LIFO reuse */
  kmem_cache_free(&c, objs[5]);
  CHECK(kmem_cache_alloc(&c) == objs[5]);

  for (uint32_t i = 0; i <= c.per_slab; i++)
    kmem_cache_free(&c, objs[i]);
  CHECK(c.inuse == 0);
  CHECK(c.nslabs == 2); /* empty slabs are kept... */
  CHECK(kmem_cache_shrink(&c) == 2); /* ...until shrunk */
  CHECK(c.nslabs == 0);
  CHECK(pmm_free_count() == pmm_start);
  CHECK(pg->flags == 0); /* page metadata reset on release */
  CHECK(bugs == 0);
}

static void test_bad_frees(void) {
  reset();
  struct kmem_cache a, b;
  kmem_cache_init(&a, "a", 64);
  kmem_cache_init(&b, "b", 64);
  uint8_t *x = kmem_cache_alloc(&a);
  uint8_t *y = kmem_cache_alloc(&a);
  int on_stack;

  EXPECT_BUG(kmem_cache_free(&a, x + 8), "middle of an object");
  EXPECT_BUG(kmem_cache_free(&b, x), "wrong cache");
  EXPECT_BUG(kmem_cache_free(&a, &on_stack), "not a slab object");
  uint8_t *page = phys_to_virt(pmm_alloc());
  EXPECT_BUG(kmem_cache_free(&a, page), "not a slab object");

  kmem_cache_free(&a, x);
  EXPECT_BUG(kmem_cache_free(&a, x), "double free");
  CHECK(a.inuse == 1); /* the bad frees changed nothing */

  kmem_cache_free(&a, NULL); /* fine */
  kmem_cache_free(&a, y);
  CHECK(a.inuse == 0);
}

/* 48-byte objects: 85 per page, 16 bytes of slack at the end */
static void test_slack(void) {
  reset();
  struct kmem_cache c;
  kmem_cache_init(&c, "48", 48);
  CHECK(c.per_slab == 85);
  uint8_t *x = kmem_cache_alloc(&c);
  uint8_t *page = (uint8_t *)((uintptr_t)x & ~(PAGE_SIZE - 1));
  /* 85 * 48 = 4080: lined up like a slot, but past the last object */
  EXPECT_BUG(kmem_cache_free(&c, page + 85 * 48), "middle of an object");
  kmem_cache_free(&c, x);
}

static void test_use_after_free_detected(void) {
  reset();
  struct kmem_cache c;
  kmem_cache_init(&c, "uaf", 64);
  uint8_t *x = kmem_cache_alloc(&c);
  uint8_t *y = kmem_cache_alloc(&c);
  kmem_cache_free(&c, x);
  /* use after free: clobber x's free-list link with an attacker pointer */
  *(void **)x = (void *)0x4141414141414141UL;
  EXPECT_BUG(kmem_cache_free(&c, y), "corrupt free list");
  /* allocating would follow the forged link: refused instead */
  void *got = (void *)1;
  EXPECT_BUG(got = kmem_cache_alloc(&c), "corrupt free list");
  CHECK(got == NULL);
}

static void test_kmalloc_classes(void) {
  reset();
  CHECK(kmalloc(0) == NULL);
  struct {
    size_t ask, got;
  } cases[] = {{1, 16},     {16, 16},     {17, 32},     {100, 128},
               {512, 512},  {513, 1024},  {2048, 2048}, {2049, 4096},
               {4096, 4096}, {4097, 8192}, {10000, 12288}};
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    uint8_t *p = kmalloc(cases[i].ask);
    CHECK(p != NULL);
    CHECK(ksize(p) == cases[i].got);
    memset(p, 0xEE, cases[i].ask); /* ASan: really that big */
    kfree(p);
  }
  kfree(NULL);
  CHECK(kmalloc_shrink() > 0);
  CHECK(pmm_free_count() == pmm_start);
  CHECK(bugs == 0);
}

static void test_kmalloc_large(void) {
  reset();
  uint8_t *p = kmalloc(3 * PAGE_SIZE);
  CHECK(p != NULL && (uintptr_t)p % PAGE_SIZE == 0);
  CHECK(pmm_free_count() == pmm_start - 3);
  struct page *pg = virt_to_page(p);
  CHECK(pg->flags == PG_LARGE && pg->count == 3);

  EXPECT_BUG(kfree(p + PAGE_SIZE), "not a slab object"); /* tail page */
  EXPECT_BUG(kfree(p + 16), "middle of an allocation");
  CHECK(ksize(p + 16) == 0);
  kfree(p);
  CHECK(pmm_free_count() == pmm_start);
  CHECK(pg->flags == 0);
  EXPECT_BUG(kfree(p), "not a slab object"); /* double free of large */
}

static void test_kzalloc(void) {
  reset();
  uint8_t *p = kmalloc(200);
  memset(p, 0xFF, 200);
  kfree(p);
  uint8_t *q = kzalloc(200);
  CHECK(q == p); /* same slot back, dirty... */
  for (size_t i = 0; i < ksize(q); i++)
    CHECK(q[i] == 0); /* ...but zeroed */
  kfree(q);
}

static void test_oom(void) {
  reset();
  while (pmm_alloc() != 0)
    ;
  CHECK(kmalloc(16) == NULL);
  CHECK(kmalloc(8192) == NULL);
  CHECK(kzalloc(64) == NULL);
}

/* Random sizes; each block carries its own pattern, checked at free. */
#define LIVE 512
static void test_random(void) {
  reset();
  static uint8_t *ptr[LIVE];
  static size_t len[LIVE];
  memset(ptr, 0, sizeof ptr);
  srand(1);
  for (int i = 0; i < 50000; i++) {
    int k = rand() % LIVE;
    if (ptr[k] != NULL) {
      for (size_t b = 0; b < len[k]; b++)
        if (ptr[k][b] != (uint8_t)(k + b)) {
          CHECK(!"block corrupted");
          break;
        }
      kfree(ptr[k]);
      ptr[k] = NULL;
    } else {
      len[k] = rand() % 4 == 0 ? (size_t)(rand() % 9000) + 1
                               : (size_t)(rand() % 300) + 1;
      ptr[k] = kmalloc(len[k]);
      CHECK(ptr[k] != NULL);
      if (ptr[k] == NULL)
        return;
      CHECK(ksize(ptr[k]) >= len[k]);
      for (size_t b = 0; b < len[k]; b++)
        ptr[k][b] = (uint8_t)(k + b);
    }
  }
  for (int k = 0; k < LIVE; k++)
    kfree(ptr[k]);
  kmalloc_shrink();
  CHECK(pmm_free_count() == pmm_start); /* nothing leaked */
  CHECK(bugs == 0);
}

int main(void) {
  /* page-aligned, like real HHDM addresses of page-aligned frames */
  mem = aligned_alloc(PAGE_SIZE, MEM_SIZE);
  test_cache_basics();
  test_bad_frees();
  test_slack();
  test_use_after_free_detected();
  test_kmalloc_classes();
  test_kmalloc_large();
  test_kzalloc();
  test_oom();
  test_random();
  free(mem);
  return CHECK_DONE();
}
