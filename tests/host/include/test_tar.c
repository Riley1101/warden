#include "check.h"
#include "tar.h"
#include "tar_builder.h"

#include <abi/errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* What tar_each reported, in order. */
struct member {
  char path[256];
  enum tar_type type;
  const void *data;
  size_t size;
};
static struct member seen[16];
static int nseen;

static int record(void *ctx, const char *path, enum tar_type type,
                  const void *data, size_t size) {
  (void)ctx;
  if (nseen == 16)
    return -1;
  struct member *m = &seen[nseen++];
  strcpy(m->path, path);
  m->type = type;
  m->data = data;
  m->size = size;
  return 0;
}

static int walk(const void *tar, size_t len) {
  nseen = 0;
  return tar_each(tar, len, record, NULL);
}

static const struct member *member(const char *path) {
  for (int i = 0; i < nseen; i++)
    if (strcmp(seen[i].path, path) == 0)
      return &seen[i];
  return NULL;
}

static void test_each(void) {
  ar_reset();
  add(NULL, "./", '5', "");
  add(NULL, "bin/", '5', "");
  add(NULL, "bin/sh", '0', "#!sh");
  add(NULL, "./etc/motd", '0', "hello\n");
  add("very/long", "path", '0', "deep");
  add(NULL, "empty", '\0', ""); /* old-style regular file */
  add(NULL, "link", '2', "");
  finish();

  CHECK(walk(ar, ar_len) == 0);
  CHECK(nseen == 6); /* the symlink is skipped */
  CHECK(strcmp(seen[0].path, "/") == 0 && seen[0].type == TAR_DIR);
  CHECK(strcmp(seen[1].path, "/bin") == 0 && seen[1].type == TAR_DIR);
  const struct member *m = member("/bin/sh");
  CHECK(m && m->type == TAR_FILE && m->size == 4 &&
        memcmp(m->data, "#!sh", 4) == 0);
  CHECK((m = member("/etc/motd")) && m->size == 6);
  CHECK((m = member("/very/long/path")) && m->size == 4);
  CHECK((m = member("/empty")) && m->size == 0 && m->type == TAR_FILE);
  CHECK(member("/link") == NULL);

  /* the callback's error stops the walk */
  nseen = 15;
  CHECK(tar_each(ar, ar_len, record, NULL) == -1);
}

static void test_malformed(void) {
  ar_reset();
  uint8_t *h = add(NULL, "a", '0', "x");
  add(NULL, "b", '0', "y");
  finish();
  h[0] = 'c'; /* checksum no longer matches */
  CHECK(walk(ar, ar_len) == -EINVAL && nseen == 0);

  ar_reset();
  add(NULL, "a", '0', "x");
  h = add(NULL, "b", '0', "y");
  finish();
  memcpy(h + 257, "nope!", 5);
  CHECK(walk(ar, ar_len) == -EINVAL && nseen == 1); /* a, then stop */

  /* size runs past the archive: never handed out */
  ar_reset();
  add(NULL, "a", '0', "x");
  size_t one = ar_len;
  CHECK(walk(ar, one - 512) == -EINVAL && nseen == 0);
  CHECK(walk(ar, 100) == 0 && nseen == 0); /* not even a header */
  CHECK(walk(NULL, 0) == 0);
  CHECK(walk(NULL, 512) == -EINVAL);

  /* a huge size field stops the walk instead of wrapping the offset */
  ar_reset();
  h = add(NULL, "a", '0', "x");
  add(NULL, "b", '0', "y");
  finish();
  memcpy(h + 124, "77777777777", 11);
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++)
    sum += h[i];
  snprintf((char *)h + 148, 8, "%06o", sum);
  CHECK(walk(ar, ar_len) == -EINVAL && member("/b") == NULL);
}

/* Random bytes never make it read outside the archive (ASan). */
static void test_garbage(void) {
  srand(2);
  for (int round = 0; round < 2000; round++) {
    ar_reset();
    add(NULL, "bin/sh", '0', "#!sh");
    add(NULL, "x", '0', "y");
    finish();
    for (int k = 0; k < 8; k++)
      ar[rand() % ar_len] = (uint8_t)rand();
    size_t len = (size_t)rand() % (ar_len + 1);
    uint8_t *copy = malloc(len ? len : 1); /* exact size: overreads trip ASan */
    memcpy(copy, ar, len);
    walk(copy, len);
    for (int i = 0; i < nseen; i++)
      CHECK((const uint8_t *)seen[i].data + seen[i].size <= copy + len);
    free(copy);
  }
}

int main(void) {
  test_each();
  test_malformed();
  test_garbage();
  return CHECK_DONE();
}
