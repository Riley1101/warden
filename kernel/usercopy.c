#include "usercopy.h"

#include <abi/errno.h>
#include <vmm.h>

int usercopy(void *dst, const void *src, size_t n); /* usercopy.asm */

struct extable_entry {
  uint64_t insn, fixup;
};

/* linker.ld */
extern const struct extable_entry __extable_start[], __extable_end[];

uint64_t extable_fixup(uint64_t rip) {
  for (const struct extable_entry *e = __extable_start; e < __extable_end; e++)
    if (e->insn == rip)
      return e->fixup;
  return 0;
}

/* [addr, addr + n) lies in the user half, without wrapping. */
static int user_range(const void *p, size_t n) {
  uint64_t addr = (uint64_t)p;
  return addr + n >= addr && addr + n <= USER_TOP;
}

int copy_from_user(void *dst, const void *usrc, size_t n) {
  if (!user_range(usrc, n))
    return -EFAULT;
  return usercopy(dst, usrc, n) ? -EFAULT : 0;
}

int copy_str_from_user(char *dst, const char *usrc, size_t size) {
  for (size_t i = 0; i < size; i++) {
    if (copy_from_user(&dst[i], usrc + i, 1) != 0)
      return -EFAULT;
    if (dst[i] == '\0')
      return (int)i;
  }
  return -ENAMETOOLONG;
}

int copy_to_user(void *udst, const void *src, size_t n) {
  if (!user_range(udst, n))
    return -EFAULT;
  return usercopy(udst, src, n) ? -EFAULT : 0;
}
