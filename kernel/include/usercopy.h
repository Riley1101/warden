#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Copy to or from the user half of the current address space. 0, or
 * -EFAULT if any byte of the user range is outside the user half, not
 * mapped, or (copy_to_user) read-only. On -EFAULT a prefix may have been
 * copied already.
 */
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);

/*
 * Copy a NUL-terminated string of at most `size` bytes (NUL included) from
 * user memory. Returns its length; -EFAULT, or -ENAMETOOLONG if no NUL
 * comes within `size` bytes.
 */
int copy_str_from_user(char *dst, const char *usrc, size_t size);

/* Where to resume a fault at `rip`, if it is a usercopy instruction; 0 if
 * not. For the #PF handler. */
uint64_t extable_fixup(uint64_t rip);
