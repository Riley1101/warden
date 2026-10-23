#include "util.h"

#include <fcntl.h>

struct counts {
  long lines, words, bytes;
};

/* Count what is left on `fd`. Words are runs of non-whitespace. */
static int count_fd(int fd, struct counts *c) {
  char buf[512];
  ssize_t n;
  int in_word = 0;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      char ch = buf[i];
      c->bytes++;
      if (ch == '\n')
        c->lines++;
      int space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
      if (space)
        in_word = 0;
      else if (!in_word) {
        in_word = 1;
        c->words++;
      }
    }
  }
  return n < 0 ? -1 : 0;
}

/* Whichever of lines, words and bytes were asked for, then `name`. */
static void report(const struct counts *c, int lines, int words, int bytes,
                   const char *name) {
  if (lines)
    printf("%8ld", c->lines);
  if (words)
    printf("%8ld", c->words);
  if (bytes)
    printf("%8ld", c->bytes);
  if (name != NULL)
    printf(" %s", name);
  printf("\n");
}

/* wc [-l] [-w] [-c] [file ...]: counts per file, then a total; stdin with
 * no files. With no flag, all three counts. */
int main(int argc, char **argv) {
  int lines = 0, words = 0, bytes = 0;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    for (const char *p = argv[i] + 1; *p != '\0'; p++) {
      switch (*p) {
      case 'l':
        lines = 1;
        break;
      case 'w':
        words = 1;
        break;
      case 'c':
        bytes = 1;
        break;
      default:
        dprintf(2, "usage: wc [-lwc] [file ...]\n");
        return 2;
      }
    }
  }
  if (!lines && !words && !bytes)
    lines = words = bytes = 1;

  if (i == argc) {
    struct counts c = {0, 0, 0};
    if (count_fd(0, &c) != 0) {
      complain("wc", "stdin", errno);
      return 1;
    }
    report(&c, lines, words, bytes, NULL);
    return 0;
  }

  struct counts total = {0, 0, 0};
  int nfiles = argc - i;
  int bad = 0;
  for (; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      complain("wc", argv[i], errno);
      bad = 1;
      continue;
    }
    struct counts c = {0, 0, 0};
    if (count_fd(fd, &c) != 0) {
      complain("wc", argv[i], errno);
      bad = 1;
    }
    close(fd);
    report(&c, lines, words, bytes, argv[i]);
    total.lines += c.lines;
    total.words += c.words;
    total.bytes += c.bytes;
  }
  if (nfiles > 1)
    report(&total, lines, words, bytes, "total");
  return bad;
}
