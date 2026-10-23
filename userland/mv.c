#include "util.h"

#include <stdio.h>

/* mv from to: rename, replacing a file (or an empty directory) at `to`.
 * There is no cwd and no "move into this directory": `to` is the new name. */
int main(int argc, char **argv) {
  if (argc != 3) {
    dprintf(2, "usage: mv from to\n");
    return 2;
  }
  if (rename(argv[1], argv[2]) != 0) {
    complain("mv", argv[1], errno);
    return 1;
  }
  return 0;
}
