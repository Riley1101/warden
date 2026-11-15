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
  case ENXIO:
    return "ENXIO";
  case E2BIG:
    return "E2BIG";
  case ENOEXEC:
    return "ENOEXEC";
  case EBADF:
    return "EBADF";
  case ECHILD:
    return "ECHILD";
  case EAGAIN:
    return "EAGAIN";
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
  case ECAPDENIED:
    return "ECAPDENIED";
  default:
    return "error";
  }
}

/* `caps` (abi/cap.h) as four letters, a '-' for each one missing, like
 * file modes: "rwsk" all, "r---" read only. `out` holds 5 bytes. */
__attribute__((unused)) static void caps_str(char *out, unsigned caps) {
  const char *letters = "rwsk"; /* CAP_FS_READ, _WRITE, _SPAWN, _KILL */
  for (int i = 0; i < 4; i++)
    out[i] = (caps & (1u << i)) ? letters[i] : '-';
  out[4] = '\0';
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
