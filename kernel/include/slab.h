#pragma once

#include <pmm.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Slab allocator: caches of fixed-size objects, one page per slab.
 *
 * All bookkeeping lives out of band in struct page (owner cache, free
 * list head, objects in use), so a slab page is nothing but objects and
 * frees can be validated against metadata a heap overflow can't reach.
 * Free objects hold the free-list link in their first 8 bytes.
 *
 * Must stay free of hardware code: it is compiled into the kernel AND into
 * host unit tests with sanitizers. No locking: callers must not race.
 */

#define SLAB_MIN_OBJ 16   /* objects are 16-byte aligned multiples of 16 */
#define SLAB_MAX_OBJ 2048 /* larger kmallocs get whole pages */

struct kmem_cache {
  const char *name;
  uint32_t size;        /* object size, rounded up to a multiple of 16 */
  uint32_t per_slab;    /* objects per page */
  struct page *partial; /* slabs with at least one free object */
  size_t nslabs;        /* pages this cache holds */
  size_t inuse;         /* objects handed out */
};

/* Set up `c` for objects of `size` bytes (1..SLAB_MAX_OBJ). */
void kmem_cache_init(struct kmem_cache *c, const char *name, size_t size);

/* Allocate one object (not zeroed), or NULL if out of memory. */
void *kmem_cache_alloc(struct kmem_cache *c);

/* Free an object from kmem_cache_alloc(c). NULL is ignored. */
void kmem_cache_free(struct kmem_cache *c, void *obj);

/* Give completely free slabs back to the PMM. Returns pages released.
 * Empty slabs are otherwise kept, so alloc/free churn stays cheap. */
size_t kmem_cache_shrink(struct kmem_cache *c);

/* General-purpose heap: power-of-two size classes 16..2048 backed by
 * slab caches, whole contiguous pages above that. */
void kmalloc_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);

/* Usable size of a kmalloc'd block (>= what was asked for), 0 if `p` is
 * not one. */
size_t ksize(const void *p);

/* kmem_cache_shrink on every kmalloc cache. Returns pages released. */
size_t kmalloc_shrink(void);

/*
 * Called on an invalid free (double free, foreign or interior pointer,
 * wrong cache). The operation is then abandoned. The kernel panics; host
 * tests record the call instead.
 */
void slab_bug(const char *what, const void *p);
