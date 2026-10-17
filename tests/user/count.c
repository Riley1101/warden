#include "t.h"

/* Read stdin to end of file; exit with how many bytes came (mod 256). */
int main(void) {
  char buf[64];
  long total = 0;
  ssize_t n;
  while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0)
    total += n;
  return n < 0 ? 255 : (int)(total & 0xff);
}
