#include <fmt.h> /* kernel/fmt.c, compiled into libc as well */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LINE_MAX 1024

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  return kvsnprintf(buf, size, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = kvsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

static int fd_vprintf(int fd, const char *fmt, va_list ap) {
  char buf[LINE_MAX];
  int n = kvsnprintf(buf, sizeof buf, fmt, ap);
  size_t len = n < (int)sizeof buf ? (size_t)n : sizeof buf - 1;
  return write(fd, buf, len) < 0 ? -1 : (int)len;
}

int dprintf(int fd, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = fd_vprintf(fd, fmt, ap);
  va_end(ap);
  return n;
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = fd_vprintf(STDOUT_FILENO, fmt, ap);
  va_end(ap);
  return n;
}

int puts(const char *s) {
  size_t n = strlen(s);
  if (write(STDOUT_FILENO, s, n) < 0 || write(STDOUT_FILENO, "\n", 1) < 0)
    return -1;
  return (int)n + 1;
}
