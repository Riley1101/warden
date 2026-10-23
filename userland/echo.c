#include "util.h"

/* echo [-n] [word ...]: the words, one space apart. -n: no final newline. */
int main(int argc, char **argv) {
  int i = 1;
  int newline = 1;
  if (argc > 1 && strcmp(argv[1], "-n") == 0) {
    newline = 0;
    i = 2;
  }
  for (; i < argc; i++) {
    if (write_all(1, argv[i], strlen(argv[i])) != 0)
      return 1;
    if (i + 1 < argc && write_all(1, " ", 1) != 0)
      return 1;
  }
  if (newline && write_all(1, "\n", 1) != 0)
    return 1;
  return 0;
}
