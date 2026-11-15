#include "check.h"
#include "fs.h"
#include "pmm.h"
#include "slab.h"
#include "tar_builder.h"

#include <abi/errno.h>
#include <stdlib.h>

/* Fake physical memory under the real slab allocator, as in test_slab.c. */
#define MEM_SIZE (32UL * 1024 * 1024)
static uint8_t *mem;

void slab_bug(const char *what, const void *p) {
  fprintf(stderr, "slab_bug: %s %p\n", what, p);
  CHECK(!"slab_bug");
}

/* Pages in use, once empty slabs are handed back: for leak checks. */
static size_t used(void) {
  kmalloc_shrink();
  return pmm_free_count();
}

static int64_t put_str(const char *path, const char *s) {
  struct node *n;
  int err = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &n);
  if (err != 0)
    return err;
  int64_t w = fs_write(n, 0, s, strlen(s));
  fs_put(n);
  return w;
}

/* The file's contents as a string ("" on error). */
static const char *get_str(const char *path) {
  static char buf[256];
  struct node *n;
  buf[0] = '\0';
  if (fs_open(path, O_RDONLY, &n) != 0)
    return buf;
  int64_t r = fs_read(n, 0, buf, sizeof buf - 1);
  buf[r > 0 ? r : 0] = '\0';
  fs_put(n);
  return buf;
}

static uint32_t type_of(const char *path) {
  struct stat st;
  return fs_stat(path, &st) == 0 ? st.type : 0;
}

static void test_files(void) {
  struct node *n;
  CHECK(fs_open("/a", O_RDONLY, &n) == -ENOENT);
  CHECK(put_str("/a", "hello") == 5);
  CHECK_STR(get_str("/a"), "hello");
  CHECK(put_str("/a", "hi") == 2); /* O_TRUNC */
  CHECK_STR(get_str("/a"), "hi");

  /* writes past the end leave zeros in the gap; reads stop at the end */
  CHECK(fs_open("/a", O_RDWR, &n) == 0);
  CHECK(fs_write(n, 6, "z", 1) == 1);
  char buf[16];
  CHECK(fs_read(n, 0, buf, sizeof buf) == 7);
  CHECK(memcmp(buf, "hi\0\0\0\0z", 7) == 0);
  CHECK(fs_read(n, 7, buf, 1) == 0);
  CHECK(fs_read(n, 1000, buf, 1) == 0);
  CHECK(fs_write(n, FS_FILE_MAX, "x", 1) == -EFBIG);
  CHECK(fs_write(n, UINT64_MAX, "x", 1) == -EFBIG);
  struct stat st;
  fs_fstat(n, &st);
  CHECK(st.type == FT_FILE && st.size == 7);
  fs_put(n);

  /* growing a file across many writes keeps every byte */
  CHECK(fs_open("/big", O_WRONLY | O_CREAT, &n) == 0);
  for (int i = 0; i < 3000; i++) {
    char c = (char)(i % 97);
    CHECK(fs_write(n, i, &c, 1) == 1);
  }
  int ok = fs_size(n) == 3000;
  for (int i = 0; i < 3000 && ok; i++) {
    char c;
    ok = fs_read(n, i, &c, 1) == 1 && c == (char)(i % 97);
  }
  CHECK(ok);
  fs_put(n);
  CHECK(fs_unlink("/big") == 0 && fs_unlink("/a") == 0);
}

static void test_dirs_and_paths(void) {
  CHECK(fs_mkdir("/d") == 0);
  CHECK(fs_mkdir("/d") == -EEXIST);
  CHECK(fs_mkdir("/") == -EEXIST);
  CHECK(fs_mkdir("/d/e") == 0);
  CHECK(fs_mkdir("/x/y") == -ENOENT); /* parent missing */
  CHECK(put_str("/d/e/f", "deep") == 4);
  CHECK(fs_mkdir("/d/e/f/g") == -ENOTDIR); /* through a file */
  CHECK(put_str("/d/e/f/g", "x") == -ENOTDIR);
  CHECK(type_of("/d") == FT_DIR && type_of("/d/e/f") == FT_FILE);

  /* every spelling of the same path reaches the same node */
  CHECK_STR(get_str("/d/./e/f"), "deep");
  CHECK_STR(get_str("//d//e/f"), "deep");
  CHECK_STR(get_str("/d/e/../e/f"), "deep");
  CHECK_STR(get_str("/../../d/e/f"), "deep"); /* .. stops at the root */
  struct node *n;
  CHECK(fs_open("d/e/f", O_RDONLY, &n) == -ENOENT); /* relative: no cwd */
  CHECK(fs_open("", O_RDONLY, &n) == -ENOENT);

  /* names: NAME_MAX fits, one more doesn't */
  char name[NAME_MAX + 3] = "/";
  memset(name + 1, 'n', NAME_MAX);
  CHECK(put_str(name, "x") == 1);
  CHECK(fs_unlink(name) == 0);
  name[NAME_MAX + 1] = 'n';
  CHECK(put_str(name, "x") == -ENAMETOOLONG);
  CHECK(fs_open(name, O_RDONLY, &n) == -ENAMETOOLONG);

  /* a directory opens read-only, and isn't read like a file */
  CHECK(fs_open("/d", O_WRONLY, &n) == -EISDIR);
  CHECK(fs_open("/d", O_RDONLY | O_TRUNC, &n) == -EISDIR);
  CHECK(fs_open("/d", O_RDONLY, &n) == 0);
  char c;
  CHECK(fs_read(n, 0, &c, 1) == -EISDIR);
  CHECK(fs_write(n, 0, &c, 1) == -EISDIR);
  struct dirent de;
  CHECK(fs_readdir(n, 0, &de) == 1 && de.type == FT_DIR);
  CHECK_STR(de.name, "e");
  CHECK(fs_readdir(n, 1, &de) == 0);
  fs_put(n);

  /* entries come back in creation order */
  CHECK(put_str("/d/z", "") == 0 && put_str("/d/a", "") == 0);
  CHECK(fs_open("/d", O_RDONLY, &n) == 0);
  const char *want[] = {"e", "z", "a"};
  for (int i = 0; i < 3; i++)
    CHECK(fs_readdir(n, i, &de) == 1 && strcmp(de.name, want[i]) == 0);
  struct stat st;
  fs_fstat(n, &st);
  CHECK(st.size == 3);
  fs_put(n);
  CHECK(fs_open("/d/z", O_RDONLY, &n) == 0);
  CHECK(fs_readdir(n, 0, &de) == -ENOTDIR);
  fs_put(n);

  /* unlink: files and empty directories; never the root */
  CHECK(fs_unlink("/d/e") == -ENOTEMPTY);
  CHECK(fs_unlink("/") == -EBUSY);
  CHECK(fs_unlink("/nope") == -ENOENT);
  CHECK(fs_unlink("/d/e/f") == 0 && fs_unlink("/d/e") == 0);
  CHECK(fs_unlink("/d/z") == 0 && fs_unlink("/d/a") == 0);
  CHECK(fs_unlink("/d") == 0);
  CHECK(type_of("/d") == 0);
}

static void test_rename(void) {
  fs_mkdir("/r");
  fs_mkdir("/r/sub");
  put_str("/r/f", "one");
  put_str("/r/g", "two");

  CHECK(fs_rename("/r/f", "/r/sub/f2") == 0); /* move and rename */
  CHECK(type_of("/r/f") == 0);
  CHECK_STR(get_str("/r/sub/f2"), "one");
  CHECK(fs_rename("/r/g", "/r/sub/f2") == 0); /* replaces a file */
  CHECK_STR(get_str("/r/sub/f2"), "two");
  CHECK(type_of("/r/g") == 0);
  CHECK(fs_rename("/r/sub/f2", "/r/sub/f2") == 0); /* onto itself */

  CHECK(fs_rename("/r", "/r/sub/inside") == -EINVAL); /* into itself */
  CHECK(fs_rename("/r", "/r") == 0);
  CHECK(fs_rename("/r/sub", "/r/sub/f2") == -EINVAL); /* into itself, too */
  put_str("/r/file", "x");
  CHECK(fs_rename("/r/sub", "/r/file") == -ENOTDIR); /* dir onto file */
  fs_unlink("/r/file");
  fs_mkdir("/r/empty");
  CHECK(fs_rename("/r/sub/f2", "/r/empty") == -EISDIR); /* file onto dir */
  CHECK(fs_rename("/r/empty", "/r/sub") == -ENOTEMPTY);
  CHECK(fs_rename("/r/sub", "/r/empty") == 0); /* dir onto empty dir */
  CHECK_STR(get_str("/r/empty/f2"), "two");
  CHECK(fs_rename("/r", "/") == -EBUSY);
  CHECK(fs_rename("/", "/x") == -EBUSY);
  CHECK(fs_rename("/nope", "/x") == -ENOENT);
  CHECK(fs_rename("/r/empty/f2", "/nodir/x") == -ENOENT);

  fs_unlink("/r/empty/f2");
  fs_unlink("/r/empty");
  CHECK(fs_unlink("/r") == 0);
}

/* An unlinked file lives while open: same contents, and freed on put. */
static void test_unlink_open(void) {
  size_t before = used();
  put_str("/u", "still here");
  struct node *n;
  CHECK(fs_open("/u", O_RDWR, &n) == 0);
  CHECK(fs_unlink("/u") == 0);
  CHECK(type_of("/u") == 0);
  char buf[16] = {0};
  CHECK(fs_read(n, 0, buf, sizeof buf) == 10);
  CHECK(strcmp(buf, "still here") == 0);
  CHECK(fs_write(n, 0, "S", 1) == 1);
  put_str("/u", "new"); /* a different file now */
  CHECK_STR(get_str("/u"), "new");
  fs_put(n);
  fs_unlink("/u");
  CHECK(used() == before);
}

static void test_tar_load(void) {
  ar_reset();
  add(NULL, "./", '5', "");
  add(NULL, "./bin/", '5', "");
  add(NULL, "./bin/sh", '0', "#!sh");
  add(NULL, "./etc/deep/motd", '0', "hi\n"); /* parents not listed */
  finish();
  CHECK(fs_load_tar(ar, ar_len) == 0);
  CHECK_STR(get_str("/bin/sh"), "#!sh");
  CHECK_STR(get_str("/etc/deep/motd"), "hi\n");
  CHECK(type_of("/etc") == FT_DIR && type_of("/etc/deep") == FT_DIR);

  /* a member where a file already sits fails, keeping what came before */
  ar_reset();
  add(NULL, "./bin/sh/x", '0', "no");
  finish();
  CHECK(fs_load_tar(ar, ar_len) == -ENOTDIR);
  CHECK_STR(get_str("/bin/sh"), "#!sh");
  ar_reset();
  add(NULL, "a", '0', "x");
  finish();
  ar[0] = 'b'; /* bad checksum */
  CHECK(fs_load_tar(ar, ar_len) == -EINVAL);
}

/* rm -r: what's below `path`, then `path` (renames can nest things at
 * paths nobody listed). */
static void rm_r(const char *path) {
  struct node *n;
  if (fs_open(path, O_RDONLY, &n) != 0)
    return;
  struct dirent de;
  while (fs_is_dir(n) && fs_readdir(n, 0, &de) == 1) {
    char sub[256];
    snprintf(sub, sizeof sub, "%s/%s", path, de.name);
    rm_r(sub);
  }
  fs_put(n);
  CHECK(fs_unlink(path) == 0);
}

/* Random operations: nothing crashes or reads outside its memory (ASan),
 * and once everything is removed every page is back. */
static void test_random(void) {
  size_t before = used();
  const char *paths[] = {"/p", "/q", "/p/a", "/p/b", "/q/a", "/p/a/x"};
  srand(4);
  for (int i = 0; i < 20000; i++) {
    const char *p = paths[rand() % 6], *q = paths[rand() % 6];
    struct node *n;
    char buf[64];
    switch (rand() % 6) {
    case 0:
      fs_mkdir(p);
      break;
    case 1:
      if (fs_open(p, O_RDWR | O_CREAT, &n) == 0) {
        if (!fs_is_dir(n))
          fs_write(n, (uint64_t)(rand() % 5000), buf, sizeof buf);
        fs_put(n);
      }
      break;
    case 2:
      fs_unlink(p);
      break;
    case 3:
      fs_rename(p, q);
      break;
    case 4:
      if (fs_open(p, O_RDONLY, &n) == 0) {
        if (!fs_is_dir(n))
          fs_read(n, (uint64_t)(rand() % 5000), buf, sizeof buf);
        fs_put(n);
      }
      break;
    default: {
      struct stat st;
      fs_stat(p, &st);
    }
    }
  }
  rm_r("/p");
  rm_r("/q");
  CHECK(type_of("/p") == 0 && type_of("/q") == 0);
  CHECK(used() == before);
}

/* The writer label of `path`, and its limit; NULL if it can't open. */
static const struct label *writer_of(const char *path, uint32_t *max) {
  struct node *n;
  if (fs_open(path, O_RDONLY, &n) != 0)
    return NULL;
  const struct label *l = fs_writer(n, max);
  fs_put(n); /* still linked: the label stays valid */
  return l;
}

static void test_writer(void) {
  struct label human = {0}, a = {0}, b = {0}, other = {0};
  CHECK(label_narrow(&a, 5, CAP_ALL, "/w") == 0);
  CHECK(label_narrow(&b, 5, CAP_FS_READ | CAP_FS_WRITE, "/w/sub") == 0);
  CHECK(label_narrow(&other, 6, CAP_ALL, "/") == 0);
  uint32_t max;

  CHECK(fs_mkdir("/w") == 0);
  CHECK(put_str("/w/f", "x") == 1);
  const struct label *l = writer_of("/w/f", &max);
  CHECK(l != NULL && l->origin == ORIGIN_HUMAN && max == 0); /* fresh: human */

  struct node *n;
  CHECK(fs_open("/w/f", O_WRONLY, &n) == 0);
  CHECK(fs_note_writer(n, &human, 0) == 0);
  CHECK(fs_writer(n, &max)->origin == ORIGIN_HUMAN);
  CHECK(fs_note_writer(n, &a, 8) == 0);
  CHECK(fs_writer(n, &max)->session == 5 && max == 8);
  /* the same session narrows it, and the lowest limit stays */
  CHECK(fs_note_writer(n, &b, 20) == 0);
  CHECK(fs_writer(n, &max)->caps == (CAP_FS_READ | CAP_FS_WRITE) && max == 8);
  CHECK_STR(fs_writer(n, &max)->scope, "/w/sub");
  /* a human's write doesn't wash it; another session can't write it */
  CHECK(fs_note_writer(n, &human, 0) == 0);
  CHECK(fs_writer(n, &max)->origin == ORIGIN_AGENT);
  CHECK(fs_note_writer(n, &other, 8) == -ECAPDENIED);
  CHECK(fs_writer(n, &max)->session == 5);
  fs_put(n);

  /* stat reports it */
  struct stat st;
  CHECK(fs_stat("/w/f", &st) == 0);
  CHECK(st.agent == 1 && st.session == 5);
  CHECK(st.caps == (CAP_FS_READ | CAP_FS_WRITE));
  CHECK(fs_stat("/w", &st) == 0 && st.agent == 0 && st.session == 0);

  /* rename moves it along; truncating by a human keeps it */
  CHECK(fs_rename("/w/f", "/g") == 0);
  l = writer_of("/g", &max);
  CHECK(l != NULL && l->origin == ORIGIN_AGENT && l->session == 5);
  CHECK(put_str("/g", "y") == 1);
  CHECK(writer_of("/g", &max)->origin == ORIGIN_AGENT);

  /* unlinking clears it: a new file there starts human */
  CHECK(fs_unlink("/g") == 0);
  CHECK(put_str("/g", "z") == 1);
  CHECK(writer_of("/g", &max)->origin == ORIGIN_HUMAN);
  CHECK(fs_unlink("/g") == 0);
  CHECK(fs_unlink("/w") == 0);
}

int main(void) {
  mem = malloc(MEM_SIZE);
  struct pmm_region r = {0x1000, MEM_SIZE - 0x1000};
  CHECK(pmm_init(&r, 1, 0, (uint64_t)(uintptr_t)mem) == 0);
  kmalloc_init();
  CHECK(fs_init() == 0);
  CHECK(type_of("/") == FT_DIR);

  test_files();
  test_dirs_and_paths();
  test_rename();
  test_unlink_open();
  test_tar_load();
  test_writer();
  test_random();
  free(mem);
  return CHECK_DONE();
}
