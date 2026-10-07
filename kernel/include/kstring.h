#pragma once

#include <stddef.h>

/*
 * The C string/memory functions the kernel needs. GCC may also emit calls
 * to memset/memcpy/memmove/memcmp on its own (struct copies, zeroing
 * loops), even with -ffreestanding, so these must exist.
 *
 * Standard prototypes, so host tests can include this and get libc's.
 */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
