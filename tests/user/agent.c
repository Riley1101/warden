#include "t.h"

/*
 * The agent channel. The kernel test feeds the bytes in (there is no
 * host on the other end here), so this side only checks that /dev/agent
 * opens, reads what arrived, and blocks rather than reporting EOF.
 */
int main(void) {
  int fd = open("/dev/agent", O_RDWR);
  if (fd < 0)
    return 1;
  if (fd != 3) /* the lowest free descriptor, like any other open */
    return 2;

  /* It is a device, not a file in the tree. */
  struct stat st;
  if (stat("/dev/agent", &st) == 0)
    return 3;
  if (!FAILS(getdent(fd, &(struct dirent){0}), ENOTDIR))
    return 4;

  /* No line discipline: a read takes whatever has arrived, newline or
   * not, which is how framed data has to come through. */
  char buf[32];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n != 8 || memcmp(buf, "ping\ntwo", 8) != 0)
    return 5;

  /* One device, however many descriptors name it. */
  if (dup2(fd, 7) != 7)
    return 6;

  /* Writing goes out of COM2, wired to nothing in this test: it should
   * still be accepted, through either descriptor. */
  if (write(fd, "pong\n", 5) != 5 || write(7, "!", 1) != 1)
    return 7;

  close(7);
  if (close(fd) != 0)
    return 8;
  return 0;
}
