#include "check.h"
#include "tar.h"

#include <abi/errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Build archives by hand: header + contents, as GNU tar --format=ustar does. */
static uint8_t ar[16 * 512];
static size_t ar_len;

static uint8_t *add(const char *prefix, const char *name, char type,
                    const char *body) {
  uint8_t *h = ar + ar_len;
  memset(h, 0, 512);
  size_t n = strlen(body);
  memcpy(h, name, strlen(name));
  if (prefix != NULL)
    memcpy(h + 345, prefix, strlen(prefix));
  snprintf((char *)h + 100, 8, "%07o", 0644);
  snprintf((char *)h + 124, 12, "%011o", (unsigned)n & 07777777);
  h[156] = type;
  memcpy(h + 257, "ustar", 6);
  memcpy(h + 263, "00", 2);
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++)
    sum += h[i];
  snprintf((char *)h + 148, 8, "%06o", sum);
  memcpy(h + 512, body, n);
  ar_len += 512 + (n + 511) / 512 * 512;
  return h;
}

static void finish(void) {
  memset(ar + ar_len, 0, 1024);
  ar_len += 1024;
}

static void reset(void) {
  memset(ar, 0, sizeof ar);
  ar_len = 0;
}

static int find(const char *path, const char **data, size_t *size) {
  return tar_find(ar, ar_len, path, (const void **)data, size);
}

static void test_find(void) {
  reset();
  add(NULL, "bin/", '5', "");
  add(NULL, "bin/sh", '0', "#!sh");
  add(NULL, "./etc/motd", '0', "hello\n");
  add("very/long", "path", '0', "deep");
  add(NULL, "empty", '0', "");
  add(NULL, "link", '2', "");
  finish();

  const char *d;
  size_t n;
  CHECK(find("/bin/sh", &d, &n) == 0 && n == 4 && memcmp(d, "#!sh", 4) == 0);
  CHECK(find("/etc/motd", &d, &n) == 0 && n == 6);
  CHECK(find("/very/long/path", &d, &n) == 0 && n == 4);
  CHECK(find("/empty", &d, &n) == 0 && n == 0);

  CHECK(find("/bin", &d, &n) == -ENOENT);      /* a directory */
  CHECK(find("/link", &d, &n) == -ENOENT);     /* not a regular file */
  CHECK(find("/bin/s", &d, &n) == -ENOENT);    /* prefix of a name */
  CHECK(find("/bin/shell", &d, &n) == -ENOENT);
  CHECK(find("bin/sh", &d, &n) == -ENOENT);    /* relative */
  CHECK(find("/very/longpath", &d, &n) == -ENOENT);
}

static void test_malformed(void) {
  const char *d;
  size_t n;

  reset();
  uint8_t *h = add(NULL, "a", '0', "x");
  finish();
  h[0] = 'b'; /* checksum no longer matches */
  CHECK(find("/b", &d, &n) == -ENOENT);

  reset();
  h = add(NULL, "a", '0', "x");
  finish();
  memcpy(h + 257, "nope!", 5);
  CHECK(find("/a", &d, &n) == -ENOENT);

  /* size runs past the archive: never handed out */
  reset();
  add(NULL, "a", '0', "x");
  size_t one = ar_len;
  CHECK(tar_find(ar, one - 512, "/a", (const void **)&d, &n) == -ENOENT);
  CHECK(tar_find(ar, 100, "/a", (const void **)&d, &n) == -ENOENT);
  CHECK(tar_find(NULL, 0, "/a", (const void **)&d, &n) == -ENOENT);

  /* a huge size field stops the walk instead of wrapping the offset */
  reset();
  h = add(NULL, "a", '0', "x");
  add(NULL, "b", '0', "y");
  finish();
  memcpy(h + 124, "77777777777", 11);
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++)
    sum += h[i];
  snprintf((char *)h + 148, 8, "%06o", sum);
  CHECK(find("/b", &d, &n) == -ENOENT);
}

/* Random bytes never make it read outside the archive (ASan). */
static void test_garbage(void) {
  srand(2);
  for (int round = 0; round < 2000; round++) {
    reset();
    add(NULL, "bin/sh", '0', "#!sh");
    add(NULL, "x", '0', "y");
    finish();
    for (int k = 0; k < 8; k++)
      ar[rand() % ar_len] = (uint8_t)rand();
    size_t len = (size_t)rand() % (ar_len + 1);
    uint8_t *copy = malloc(len ? len : 1); /* exact size: overreads trip ASan */
    memcpy(copy, ar, len);
    const void *d;
    size_t n;
    if (tar_find(copy, len, "/x", &d, &n) == 0)
      CHECK((const uint8_t *)d + n <= copy + len);
    free(copy);
  }
}

int main(void) {
  test_find();
  test_malformed();
  test_garbage();
  return CHECK_DONE();
}
