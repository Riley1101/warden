#include "check.h"
#include "path.h"

#include <abi/errno.h>
#include <stdlib.h>

static void ok(const char *in, const char *want) {
  char out[PATH_MAX];
  int r = path_normalize(in, out, sizeof out);
  if (r != 0) {
    fprintf(stderr, "path_normalize(\"%s\") returned %d\n", in, r);
    check_failures++;
    return;
  }
  CHECK_STR(out, want);
}

static void err(const char *in, size_t out_size, int want) {
  char out[PATH_MAX];
  int r = path_normalize(in, out, out_size);
  if (r != want) {
    fprintf(stderr, "path_normalize(\"%s\", size %zu) = %d, want %d\n", in,
            out_size, r, want);
    check_failures++;
  }
}

static void test_basic(void) {
  ok("/", "/");
  ok("/a", "/a");
  ok("/a/b/c", "/a/b/c");
}

static void test_slashes(void) {
  ok("//", "/");
  ok("/a//b", "/a/b");
  ok("/a/b/", "/a/b");
  ok("///a///b///", "/a/b");
}

static void test_dot(void) {
  ok("/.", "/");
  ok("/a/./b", "/a/b");
  ok("/./a/.", "/a");
}

static void test_dotdot(void) {
  ok("/a/..", "/");
  ok("/a/../b", "/b");
  ok("/a/b/../c", "/a/c");
  ok("/a/b/c/../../..", "/");
}

/* The cases that matter for security: you can't climb above the root,
 * and a scoped prefix like /home is escaped lexically. */
static void test_escape(void) {
  ok("/..", "/");
  ok("/../..", "/");
  ok("/../etc", "/etc");
  ok("/home/../bin", "/bin");
  ok("/home/user/../../etc/passwd", "/etc/passwd");
  ok("/home/./../bin", "/bin");
}

/* Names that only look special. */
static void test_not_special(void) {
  ok("/...", "/...");
  ok("/a/..b", "/a/..b");
  ok("/a/b..", "/a/b..");
  ok("/.hidden", "/.hidden");
}

static void test_errors(void) {
  err("", PATH_MAX, -EINVAL);
  err("a/b", PATH_MAX, -EINVAL);
  err("./a", PATH_MAX, -EINVAL);
  err("/abc", 4, -ENAMETOOLONG); /* needs 5 bytes */
  err("/abc", 5, 0);             /* fits exactly */
  err("/", 1, -EINVAL);          /* buffer too small for anything */
}

/* Property test: for random inputs the output must always be canonical,
 * and normalizing twice must change nothing. */
static void test_random(void) {
  static const char alphabet[] = "/./..ab";
  unsigned seed = 12345;

  for (int iter = 0; iter < 100000; iter++) {
    char in[64];
    int n = 1 + (int)(seed % 40);
    in[0] = '/';
    for (int i = 1; i < n; i++) {
      seed = seed * 1103515245u + 12345u;
      in[i] = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
    }
    in[n] = '\0';
    seed = seed * 1103515245u + 12345u;

    char out[PATH_MAX], again[PATH_MAX];
    if (path_normalize(in, out, sizeof out) != 0) {
      fprintf(stderr, "random: \"%s\" failed\n", in);
      check_failures++;
      continue;
    }
    size_t len = strlen(out);
    CHECK(out[0] == '/');
    CHECK(strstr(out, "//") == NULL);
    CHECK(strstr(out, "/./") == NULL);
    CHECK(strstr(out, "/../") == NULL);
    CHECK(len == 1 || out[len - 1] != '/');
    CHECK(!(len >= 2 && strcmp(out + len - 2, "/.") == 0));
    CHECK(!(len >= 3 && strcmp(out + len - 3, "/..") == 0));

    CHECK(path_normalize(out, again, sizeof again) == 0);
    CHECK_STR(again, out);

    if (check_failures) {
      fprintf(stderr, "  input was \"%s\"\n", in);
      return;
    }
  }
}

int main(void) {
  test_basic();
  test_slashes();
  test_dot();
  test_dotdot();
  test_escape();
  test_not_special();
  test_errors();
  test_random();
  return CHECK_DONE();
}
