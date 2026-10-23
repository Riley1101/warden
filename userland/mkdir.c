#include "util.h"

#include <sys/stat.h>

/* mkdir dir ...: one level each; the parent must exist. */
int main(int argc, char **argv) {
  if (argc < 2) {
    dprintf(2, "usage: mkdir dir ...\n");
    return 2;
  }
  int bad = 0;
  for (int i = 1; i < argc; i++) {
    if (mkdir(argv[i]) != 0) {
      complain("mkdir", argv[i], errno);
      bad = 1;
    }
  }
  return bad;
}
