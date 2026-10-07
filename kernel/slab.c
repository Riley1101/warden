#include "slab.h"

#include <kstring.h>

/* ---- slab caches ---------------------------------------------------------- */

void kmem_cache_init(struct kmem_cache *c, const char *name, size_t size) {
  if (size < SLAB_MIN_OBJ)
    size = SLAB_MIN_OBJ;
  size = (size + SLAB_MIN_OBJ - 1) & ~(size_t)(SLAB_MIN_OBJ - 1);
  c->name = name;
  c->size = (uint32_t)size;
  c->per_slab = size <= SLAB_MAX_OBJ ? (uint32_t)(PAGE_SIZE / size) : 0;
  c->partial = NULL;
  c->nslabs = 0;
  c->inuse = 0;
}

static uint8_t *slab_base(const struct page *pg) {
  return phys_to_virt(page_to_phys(pg));
}

/* Free objects store the next free object in their first word. */
static void *next_free(void *obj) { return *(void **)obj; }
static void set_next_free(void *obj, void *next) { *(void **)obj = next; }

/* Is `p` the start of an object slot in this slab? Free-list links live in
 * freed objects, where a use-after-free can overwrite them, so every link
 * is checked with this before it's followed. */
static int is_slot(const struct page *pg, const void *p) {
  const struct kmem_cache *c = pg->owner;
  const uint8_t *base = slab_base(pg);
  if ((const uint8_t *)p < base)
    return 0;
  size_t off = (size_t)((const uint8_t *)p - base);
  return off % c->size == 0 && off / c->size < c->per_slab;
}

static struct page *new_slab(struct kmem_cache *c) {
  uint64_t phys = pmm_alloc();
  if (phys == 0)
    return NULL;
  struct page *pg = phys_to_page(phys);
  uint8_t *base = phys_to_virt(phys);

  /* Thread the free list through the page in address order. */
  for (uint32_t i = 0; i < c->per_slab; i++)
    set_next_free(base + i * c->size,
                  i + 1 < c->per_slab ? base + (i + 1) * c->size : NULL);

  pg->flags = PG_SLAB;
  pg->owner = c;
  pg->freelist = base;
  pg->count = 0;
  pg->next = NULL;
  c->nslabs++;
  return pg;
}

void *kmem_cache_alloc(struct kmem_cache *c) {
  if (c->per_slab == 0)
    return NULL;
  struct page *pg = c->partial;
  if (pg == NULL) {
    pg = new_slab(c);
    if (pg == NULL)
      return NULL;
    c->partial = pg;
  }

  void *obj = pg->freelist;
  void *next = next_free(obj);
  if (next != NULL && !is_slot(pg, next)) {
    slab_bug("corrupt free list (use after free?)", obj);
    return NULL;
  }
  pg->freelist = next;
  pg->count++;
  c->inuse++;
  if (pg->freelist == NULL) { /* now full: off the partial list */
    c->partial = pg->next;
    pg->next = NULL;
  }
  return obj;
}

/*
 * Check that `obj` is the start of an allocated object in a slab (of
 * `expect`, if given). Returns its page, or NULL after reporting a bug.
 * Everything checked lives in struct page, not next to the object.
 */
static struct page *check_obj(const void *obj, const struct kmem_cache *expect) {
  struct page *pg = virt_to_page(obj);
  if (pg == NULL || !(pg->flags & PG_SLAB)) {
    slab_bug("free of a pointer that is not a slab object", obj);
    return NULL;
  }
  const struct kmem_cache *c = pg->owner;
  if (expect != NULL && c != expect) {
    slab_bug("free to the wrong cache", obj);
    return NULL;
  }
  if (!is_slot(pg, obj)) {
    slab_bug("free of a pointer into the middle of an object", obj);
    return NULL;
  }
  /* Double free: is it already on the free list? Bounded by per_slab, so a
   * corrupted list can't loop forever. */
  uint32_t steps = 0;
  for (void *f = pg->freelist; f != NULL; f = next_free(f)) {
    if (!is_slot(pg, f) || ++steps > c->per_slab) {
      slab_bug("corrupt free list (use after free?)", obj);
      return NULL;
    }
    if (f == obj) {
      slab_bug("double free", obj);
      return NULL;
    }
  }
  return pg;
}

static void free_obj(struct page *pg, void *obj) {
  struct kmem_cache *c = pg->owner;
  int was_full = pg->freelist == NULL;
  set_next_free(obj, pg->freelist);
  pg->freelist = obj;
  pg->count--;
  c->inuse--;
  if (was_full) { /* has room again: back on the partial list */
    pg->next = c->partial;
    c->partial = pg;
  }
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
  if (obj == NULL)
    return;
  struct page *pg = check_obj(obj, c);
  if (pg != NULL)
    free_obj(pg, obj);
}

size_t kmem_cache_shrink(struct kmem_cache *c) {
  size_t released = 0;
  struct page **link = &c->partial;
  while (*link != NULL) {
    struct page *pg = *link;
    if (pg->count == 0) {
      *link = pg->next;
      pmm_free(page_to_phys(pg)); /* also resets the struct page */
      c->nslabs--;
      released++;
    } else {
      link = &pg->next;
    }
  }
  return released;
}

/* ---- kmalloc -------------------------------------------------------------- */

#define NCLASSES 8 /* 16, 32, ..., 2048 */

static struct kmem_cache classes[NCLASSES];
static const char *const class_names[NCLASSES] = {
    "kmalloc-16",  "kmalloc-32",  "kmalloc-64",   "kmalloc-128",
    "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048",
};

_Static_assert((SLAB_MIN_OBJ << (NCLASSES - 1)) == SLAB_MAX_OBJ,
               "size classes must end at SLAB_MAX_OBJ");

void kmalloc_init(void) {
  for (int i = 0; i < NCLASSES; i++)
    kmem_cache_init(&classes[i], class_names[i], (size_t)SLAB_MIN_OBJ << i);
}

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;
  if (size <= SLAB_MAX_OBJ) {
    int i = 0;
    while (((size_t)SLAB_MIN_OBJ << i) < size)
      i++;
    return kmem_cache_alloc(&classes[i]);
  }

  /* Large: whole contiguous pages, recorded on the first page. */
  if (size > (size_t)UINT32_MAX * PAGE_SIZE)
    return NULL;
  size_t n = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t phys = pmm_alloc_pages(n);
  if (phys == 0)
    return NULL;
  struct page *pg = phys_to_page(phys);
  pg->flags = PG_LARGE;
  pg->count = (uint32_t)n;
  return phys_to_virt(phys);
}

void *kzalloc(size_t size) {
  void *p = kmalloc(size);
  if (p != NULL)
    memset(p, 0, ksize(p));
  return p;
}

void kfree(void *p) {
  if (p == NULL)
    return;
  struct page *pg = virt_to_page(p);
  if (pg != NULL && (pg->flags & PG_LARGE)) {
    if (p != slab_base(pg)) {
      slab_bug("kfree of a pointer into the middle of an allocation", p);
      return;
    }
    if (pmm_free_pages(page_to_phys(pg), pg->count) != 0)
      slab_bug("kfree of a corrupt large allocation", p);
    return;
  }
  /* Any slab object, kmalloc'd or from a dedicated cache. */
  pg = check_obj(p, NULL);
  if (pg != NULL)
    free_obj(pg, p);
}

size_t ksize(const void *p) {
  const struct page *pg = virt_to_page(p);
  if (pg == NULL)
    return 0;
  if (pg->flags & PG_SLAB)
    return ((const struct kmem_cache *)pg->owner)->size;
  if ((pg->flags & PG_LARGE) && p == slab_base(pg))
    return (size_t)pg->count * PAGE_SIZE;
  return 0;
}

size_t kmalloc_shrink(void) {
  size_t released = 0;
  for (int i = 0; i < NCLASSES; i++)
    released += kmem_cache_shrink(&classes[i]);
  return released;
}
