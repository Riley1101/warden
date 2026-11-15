#include "util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

/*
 * ls [-l] [path ...]: the entries of each directory, the name of each
 * file, one per line: ls's output is meant to be piped. With more than one
 * argument, each listing gets a "path:" header.
 *
 * -l puts what stat knows in front of each name:
 *
 *   f agent:1 rw--     12 notes
 *   d human   ----      3 bin
 *
 * type (f file, d directory); who wrote it, "agent:SESSION" if an agent
 * did; the caps it runs with when exec'd, as caps_str spells them ("----"
 * for a human's: nothing is taken away); size in bytes, or entries for a
 * directory.
 */

#define NAME_PATH 320 /* a directory's path, '/', and one name */

static int long_form;

static void print_entry(const char *name, const struct stat *st) {
  if (!long_form) {
    printf("%s\n", name);
    return;
  }
  char writer[16], caps[5];
  if (st->agent)
    snprintf(writer, sizeof writer, "agent:%u", (unsigned)st->session);
  else
    snprintf(writer, sizeof writer, "human");
  caps_str(caps, st->agent ? st->caps : 0);
  printf("%c %-8s %s %6lu %s\n", st->type == FT_DIR ? 'd' : 'f', writer, caps,
         (unsigned long)st->size, name);
}

static int list_dir(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    complain("ls", path, errno);
    return 1;
  }
  int bad = 0;
  struct dirent d;
  int r;
  while ((r = getdent(fd, &d)) == 1) {
    if (strcmp(d.name, ".") == 0 || strcmp(d.name, "..") == 0)
      continue;
    struct stat st = {.type = d.type};
    if (long_form) {
      char full[NAME_PATH];
      snprintf(full, sizeof full, "%s%s%s", path,
               path[strlen(path) - 1] == '/' ? "" : "/", d.name);
      if (stat(full, &st) != 0) {
        complain("ls", full, errno);
        bad = 1;
        continue;
      }
    }
    print_entry(d.name, &st);
  }
  if (r < 0)
    complain("ls", path, errno);
  close(fd);
  return bad || r < 0;
}

int main(int argc, char **argv) {
  int first = 1;
  for (; first < argc && argv[first][0] == '-' && argv[first][1] != '\0';
       first++) {
    if (strcmp(argv[first], "-l") != 0) {
      dprintf(2, "usage: ls [-l] [path ...]\n");
      return 2;
    }
    long_form = 1;
  }

  const char *root = "/";
  char *const *paths = first < argc ? &argv[first] : (char *const *)&root;
  int n = first < argc ? argc - first : 1;

  int bad = 0;
  for (int i = 0; i < n; i++) {
    struct stat st;
    if (stat(paths[i], &st) != 0) {
      complain("ls", paths[i], errno);
      bad = 1;
      continue;
    }
    if (st.type != FT_DIR) {
      print_entry(paths[i], &st);
      continue;
    }
    if (n > 1)
      printf("%s%s:\n", i > 0 ? "\n" : "", paths[i]);
    bad |= list_dir(paths[i]);
  }
  return bad;
}
