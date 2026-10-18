#include "t.h"

/* `path`'s contents into buf (NUL-terminated); its length, or -1. */
static long slurp(const char *path, char *buf, size_t size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  long got = 0;
  ssize_t n;
  while (got < (long)size - 1 && (n = read(fd, buf + got, size - 1 - got)) > 0)
    got += n;
  close(fd);
  buf[got] = '\0';
  return got;
}

static int spit(const char *path, const char *s, int flags) {
  int fd = open(path, O_WRONLY | O_CREAT | flags);
  if (fd < 0)
    return -1;
  ssize_t w = write(fd, s, strlen(s));
  close(fd);
  return w == (ssize_t)strlen(s) ? 0 : -1;
}

/* Is `name` an entry of directory `dir`? */
static int listed(const char *dir, const char *name) {
  int fd = open(dir, O_RDONLY);
  struct dirent d;
  int found = 0;
  while (fd >= 0 && getdent(fd, &d) == 1)
    found |= strcmp(d.name, name) == 0;
  close(fd);
  return found;
}

int main(void) {
  char buf[256];

  /* The initramfs is in the tree. */
  struct stat st;
  if (stat("/test", &st) != 0 || st.type != FT_DIR)
    return 1;
  if (stat("/test/hello", &st) != 0 || st.type != FT_FILE || st.size == 0)
    return 2;
  if (!listed("/test", "hello") || !listed("/", "test"))
    return 3;

  /* Create, write, read back; truncate and append. */
  if (mkdir("/tmp") != 0 || !FAILS(mkdir("/tmp"), EEXIST))
    return 4;
  if (spit("/tmp/a", "hello", O_TRUNC) != 0 || slurp("/tmp/a", buf, sizeof buf) != 5 ||
      strcmp(buf, "hello") != 0)
    return 5;
  if (spit("/tmp/a", " world", O_APPEND) != 0 ||
      strcmp((slurp("/tmp/a", buf, sizeof buf), buf), "hello world") != 0)
    return 6;
  if (spit("/tmp/a", "new", O_TRUNC) != 0 ||
      strcmp((slurp("/tmp/a", buf, sizeof buf), buf), "new") != 0)
    return 7;

  /* Errors. */
  if (!FAILS(open("/tmp/none", O_RDONLY), ENOENT) ||
      !FAILS(open("/tmp/a/x", O_RDONLY | O_CREAT), ENOTDIR) ||
      !FAILS(open("/tmp", O_WRONLY), EISDIR) ||
      !FAILS(open("tmp/a", O_RDONLY), ENOENT)) /* relative: no cwd */
    return 8;
  int fd = open("/tmp/a", O_RDONLY);
  if (!FAILS(write(fd, "x", 1), EBADF))
    return 9;
  char *volatile bad = (char *)8;
  if (!FAILS(read(fd, bad, 1), EFAULT) || !FAILS(open(bad, O_RDONLY), EFAULT))
    return 10;
  close(fd);
  fd = open("/tmp", O_RDONLY);
  if (!FAILS(read(fd, buf, 1), EISDIR))
    return 11;
  close(fd);
  fd = open("/tmp/a", O_RDONLY);
  struct dirent d;
  if (!FAILS(getdent(fd, &d), ENOTDIR))
    return 12;
  close(fd);

  /* Rename and unlink. */
  if (rename("/tmp/a", "/tmp/b") != 0 || stat("/tmp/a", &st) == 0 ||
      !listed("/tmp", "b"))
    return 13;
  if (!FAILS(unlink("/tmp"), ENOTEMPTY))
    return 14;

  /* An unlinked file stays readable through an open descriptor. */
  fd = open("/tmp/b", O_RDONLY);
  if (unlink("/tmp/b") != 0 || stat("/tmp/b", &st) == 0)
    return 15;
  if (read(fd, buf, sizeof buf) != 3 || memcmp(buf, "new", 3) != 0)
    return 16;
  close(fd);
  if (unlink("/tmp") != 0)
    return 17;

  /* Fork shares the offset: the child's write moves the parent's. */
  mkdir("/tmp");
  fd = open("/tmp/shared", O_RDWR | O_CREAT);
  write(fd, "ab", 2);
  pid_t pid = fork();
  if (pid == 0)
    _exit(write(fd, "cd", 2) == 2 ? 0 : 1);
  int status;
  wait(&status);
  write(fd, "ef", 2);
  close(fd);
  if (status != W_EXITCODE(0) ||
      strcmp((slurp("/tmp/shared", buf, sizeof buf), buf), "abcdef") != 0)
    return 18;

  /* Programs run from the filesystem: a copy made at run time runs. */
  long n = slurp("/test/hello", buf, 1); /* just checks it opens */
  int in = open("/test/hello", O_RDONLY);
  int out = open("/tmp/hello2", O_WRONLY | O_CREAT);
  static char copy[64 * 1024];
  ssize_t k;
  while ((k = read(in, copy, sizeof copy)) > 0)
    write(out, copy, (size_t)k);
  close(in);
  close(out);
  pid = fork();
  if (pid == 0) {
    execv("/tmp/hello2", NULL);
    _exit(99);
  }
  if (n < 0 || wait(&status) != pid || status != W_EXITCODE(7))
    return 19;
  if (!FAILS(execv("/tmp", NULL), EACCES)) /* a directory */
    return 20;

  /* Leave the tree as we found it: the kernel test then checks every
   * page came back. */
  if (unlink("/tmp/shared") != 0 || unlink("/tmp/hello2") != 0 ||
      unlink("/tmp") != 0)
    return 21;
  return 0;
}
