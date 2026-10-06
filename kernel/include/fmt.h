#pragma once

#include <stdarg.h>
#include <stddef.h>

/*
 * Minimal printf-style formatter.
 *
 * Supports: %d %i %u %x %p %s %c %%, the `l`/`ll` length modifiers,
 * a field width, and the `0` (zero-pad) and `-` (left-justify) flags.
 * Unknown conversions are emitted literally ("%q" -> "%q").
 */
int kvsnprintf(char *buf, size_t buf_size, const char *fmt, va_list args);

/**
 * @brief Format a string into a buffer.
 */
int ksnprintf(char *buf, size_t buf_size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
