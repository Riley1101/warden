#pragma once

/* Shared by the ring 3 test programs (no libc yet). Each program reports
 * through its exit code: 0 passes, anything else is the failing step. */

#include <sys.h>

static inline int streq(const char *a, const char *b) {
  while (*a != '\0' && *a == *b)
    a++, b++;
  return *a == *b;
}

static inline void dputs(const char *s) {
  size_t n = 0;
  while (s[n] != '\0')
    n++;
  sys_debug_write(s, n);
}
