#include "t.h"

/* echo [words...]: the words, space separated, to stdout. */
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    if (printf(i > 1 ? " %s" : "%s", argv[i]) < 0)
      return 1;
  return puts("") < 0 ? 1 : 0;
}
