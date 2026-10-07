#include "kstring.h"

/*
 * Kernel only (host tests use libc's). Written with rep stos/movs rather
 * than C loops: GCC can recognise a byte loop as "memset" and compile it
 * into a call to memset -- from inside memset.
 */

void *memset(void *dst, int c, size_t n) {
  void *d = dst;
  __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
  void *d = dst;
  __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  if ((const char *)dst <= (const char *)src ||
      (const char *)dst >= (const char *)src + n)
    return memcpy(dst, src, n); /* no harmful overlap: copy forwards */

  /* dst overlaps the end of src: copy backwards (DF=1), last byte first */
  void *d = (char *)dst + n - 1;
  const void *s = (const char *)src + n - 1;
  __asm__ __volatile__("std\n\t"
                       "rep movsb\n\t"
                       "cld"
                       : "+D"(d), "+S"(s), "+c"(n)
                       :
                       : "memory");
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return x[i] - y[i];
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0')
    n++;
  return n;
}
