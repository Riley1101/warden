#pragma once

#include <stdarg.h>
#include <stddef.h>

/*
 * Formatted output, unbuffered: each call is one write(). Conversions:
 * %d %i %u %x %p %s %c %%, with l/ll, a width, and the 0 and - flags.
 * printf and dprintf format into a 1 KiB buffer: longer output is cut.
 */
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* `s` and a newline to stdout. */
int puts(const char *s);

/* Move `from` to `to`, replacing a file (or an empty directory) there. */
int rename(const char *from, const char *to);
