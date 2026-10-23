#include "util.h"

/* rm file ...: unlink each one (a directory must be empty). */
int main(int argc, char **argv) {
  if (argc < 2) {
    dprintf(2, "usage: rm file ...\n");
    return 2;
  }
  int bad = 0;
  for (int i = 1; i < argc; i++) {
    if (unlink(argv[i]) != 0) {
      complain("rm", argv[i], errno);
      bad = 1;
    }
  }
  return bad;
}
