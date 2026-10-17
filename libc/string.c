#include <stdlib.h>
#include <string.h>

/* memset, memcpy, memmove, memcmp and strlen are kernel/kstring.c,
 * compiled into libc as well. */

int strcmp(const char *a, const char *b) {
  while (*a != '\0' && *a == *b)
    a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (; n > 0; n--, a++, b++) {
    if (*a != *b)
      return (unsigned char)*a - (unsigned char)*b;
    if (*a == '\0')
      return 0;
  }
  return 0;
}

char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (*s == '\0')
      return NULL;
  }
}

char *strrchr(const char *s, int c) {
  const char *last = NULL;
  for (;; s++) {
    if (*s == (char)c)
      last = s;
    if (*s == '\0')
      return (char *)last;
  }
}

char *strcpy(char *dst, const char *src) {
  memcpy(dst, src, strlen(src) + 1);
  return dst;
}

char *strcat(char *dst, const char *src) {
  strcpy(dst + strlen(dst), src);
  return dst;
}

size_t strlcpy(char *dst, const char *src, size_t n) {
  size_t len = strlen(src);
  if (n > 0) {
    size_t k = len < n - 1 ? len : n - 1;
    memcpy(dst, src, k);
    dst[k] = '\0';
  }
  return len;
}

char *strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  if (d != NULL)
    memcpy(d, s, n);
  return d;
}
