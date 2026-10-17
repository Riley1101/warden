#include "t.h"

/* The first C program in ring 3: prints, and exits with a known code. */
int main(void) {
  dputs("hello from ring 3\n");
  return 7;
}
