#pragma once

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Shared by the programs in /bin. Each is one source file linked against
 * libc, so what they have in common lives here, in the header.
 */

/* The name of an errno value, for messages: "ENOENT". */
__attribute__((unused)) static const char *errname(int e) {
  switch (e) {
  case EPERM:
    return "EPERM";
  case ENOENT:
    return "ENOENT";
  case ESRCH:
    return "ESRCH";
  case E2BIG:
    return "E2BIG";
  case ENOEXEC:
    return "ENOEXEC";
  case EBADF:
    return "EBADF";
  case ECHILD:
    return "ECHILD";
  case ENOMEM:
    return "ENOMEM";
  case EACCES:
    return "EACCES";
  case EFAULT:
    return "EFAULT";
  case EBUSY:
    return "EBUSY";
  case EEXIST:
    return "EEXIST";
  case ENOTDIR:
    return "ENOTDIR";
  case EISDIR:
    return "EISDIR";
  case EINVAL:
    return "EINVAL";
  case EMFILE:
    return "EMFILE";
  case EFBIG:
    return "EFBIG";
  case EPIPE:
    return "EPIPE";
  case ENAMETOOLONG:
    return "ENAMETOOLONG";
  case ENOSYS:
    return "ENOSYS";
  case ENOTEMPTY:
    return "ENOTEMPTY";
  default:
    return "error";
  }
}

/* "prog: what: ERRNO" on stderr. */
__attribute__((unused)) static void complain(const char *prog, const char *what,
                                             int e) {
  dprintf(2, "%s: %s: %s\n", prog, what, errname(e));
}

/* `n` bytes of `buf` to `fd`, however many writes that takes. */
__attribute__((unused)) static int write_all(int fd, const char *buf,
                                             size_t n) {
  for (size_t done = 0; done < n;) {
    ssize_t w = write(fd, buf + done, n - done);
    if (w <= 0)
      return -1;
    done += (size_t)w;
  }
  return 0;
}
