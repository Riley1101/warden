#include "util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

/* One name per line: ls's output is meant to be piped. */
static int list_dir(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    complain("ls", path, errno);
    return 1;
  }
  struct dirent d;
  int r;
  while ((r = getdent(fd, &d)) == 1) {
    if (strcmp(d.name, ".") == 0 || strcmp(d.name, "..") == 0)
      continue;
    printf("%s\n", d.name);
  }
  if (r < 0)
    complain("ls", path, errno);
  close(fd);
  return r < 0;
}

/* ls [path ...]: the entries of each directory, the name of each file.
 * With more than one argument, each listing gets a "path:" header. */
int main(int argc, char **argv) {
  const char *root = "/";
  char *const *paths = argc > 1 ? &argv[1] : (char *const *)&root;
  int n = argc > 1 ? argc - 1 : 1;

  int bad = 0;
  for (int i = 0; i < n; i++) {
    struct stat st;
    if (stat(paths[i], &st) != 0) {
      complain("ls", paths[i], errno);
      bad = 1;
      continue;
    }
    if (st.type != FT_DIR) {
      printf("%s\n", paths[i]);
      continue;
    }
    if (n > 1)
      printf("%s%s:\n", i > 0 ? "\n" : "", paths[i]);
    bad |= list_dir(paths[i]);
  }
  return bad;
}
