#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Nothing to flush (stdio is unbuffered), no atexit. */
void exit(int code) { _exit(code); }

int atoi(const char *s) {
  int sign = 1, v = 0;
  if (*s == '-' || *s == '+')
    sign = *s++ == '-' ? -1 : 1;
  for (; *s >= '0' && *s <= '9'; s++)
    v = v * 10 + (*s - '0');
  return sign * v;
}

/* malloc.c found a corrupt heap or a bad free: stop before it spreads. */
void malloc_bug(const char *what, const void *p) {
  dprintf(STDERR_FILENO, "malloc: %s: %p\n", what, p);
  _exit(134);
}
