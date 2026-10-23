#include "util.h"

#include <fcntl.h>

/* Everything left on `fd` to stdout. 0 on success. */
static int cat_fd(int fd, const char *name) {
  char buf[512];
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    if (write_all(1, buf, (size_t)n) != 0) {
      complain("cat", "stdout", errno);
      return 1;
    }
  }
  if (n < 0) {
    complain("cat", name, errno);
    return 1;
  }
  return 0;
}

/* cat [file ...]: the files, or stdin with no arguments. */
int main(int argc, char **argv) {
  if (argc < 2)
    return cat_fd(0, "stdin");

  int bad = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      complain("cat", argv[i], errno);
      bad = 1;
      continue;
    }
    bad |= cat_fd(fd, argv[i]);
    close(fd);
  }
  return bad;
}
