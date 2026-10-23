/* Force-included when libc/malloc.c is host-tested: its functions get
 * their own names, so they don't replace the host's malloc, and sbrk is
 * the test's fake.
 *
 * The host's headers come first: the renames must not rewrite the system
 * declarations (macOS declares sbrk(int), glibc sbrk(intptr_t), and either
 * one would then clash with the fake). The new names get prototypes here
 * instead, since the system headers no longer supply any. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define malloc  lc_malloc
#define free    lc_free
#define calloc  lc_calloc
#define realloc lc_realloc
#define sbrk    lc_sbrk

void *lc_malloc(size_t size);
void lc_free(void *p);
void *lc_calloc(size_t n, size_t size);
void *lc_realloc(void *p, size_t size);
void *lc_sbrk(intptr_t inc);
