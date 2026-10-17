/* Force-included when libc/malloc.c is host-tested: its functions get
 * their own names, so they don't replace the host's malloc, and sbrk is
 * the test's fake. */
#define malloc  lc_malloc
#define free    lc_free
#define calloc  lc_calloc
#define realloc lc_realloc
#define sbrk    lc_sbrk
