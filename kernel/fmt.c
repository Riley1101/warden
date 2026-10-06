#include "fmt.h"

#include <stdint.h>

/**
 * @brief A sink for formatted output.
 */
struct sink {
  char *buf;
  size_t size;
  size_t pos;
};

/**
 * @brief Write a character to the sink.
 */
static void sink_putc(struct sink *s, char c) {
  if (s->pos + 1 < s->size)
    s->buf[s->pos] = c;
  s->pos++;
}

/**
 * @brief Write `n` copies of `c` to the sink.
 */
static void sink_pad(struct sink *s, char c, int n) {
  while (n-- > 0)
    sink_putc(s, c);
}

struct spec {
  int zero;  /* '0' flag */
  int left;  /* '-' flag */
  int width; /* minimum field width */
};

static void put_str(struct sink *s, const char *str, const struct spec *sp) {
  if (str == NULL)
    str = "(null)";
  int len = 0;
  while (str[len] != '\0')
    len++;
  if (!sp->left)
    sink_pad(s, ' ', sp->width - len);
  while (*str)
    sink_putc(s, *str++);
  if (sp->left)
    sink_pad(s, ' ', sp->width - len);
}

/*
 * Emit `v` in `base`, with an optional sign and prefix ("0x") that count
 * toward the field width. Zero padding goes between the prefix and digits.
 */
static void put_uint(struct sink *s, unsigned long v, unsigned base, int neg,
                     const char *prefix, const struct spec *sp) {
  static const char digits[] = "0123456789abcdef";
  char tmp[20]; /* 2^64 - 1 is 20 decimal digits */
  int n = 0;
  do {
    tmp[n++] = digits[v % base];
    v /= base;
  } while (v != 0);

  int plen = 0;
  while (prefix[plen] != '\0')
    plen++;
  int pad = sp->width - n - plen - (neg ? 1 : 0);

  if (!sp->left && !sp->zero)
    sink_pad(s, ' ', pad);
  if (neg)
    sink_putc(s, '-');
  for (int i = 0; i < plen; i++)
    sink_putc(s, prefix[i]);
  if (!sp->left && sp->zero)
    sink_pad(s, '0', pad);
  while (n > 0)
    sink_putc(s, tmp[--n]);
  if (sp->left)
    sink_pad(s, ' ', pad);
}

int kvsnprintf(char *buf, size_t buf_size, const char *fmt, va_list args) {
  struct sink s = {buf, buf_size, 0};

  while (*fmt != '\0') {
    if (*fmt != '%') {
      sink_putc(&s, *fmt++);
      continue;
    }
    const char *start = fmt++; /* remember "%" for unknown conversions */

    struct spec sp = {0, 0, 0};
    for (;; fmt++) {
      if (*fmt == '0')
        sp.zero = 1;
      else if (*fmt == '-')
        sp.left = 1;
      else
        break;
    }
    while (*fmt >= '0' && *fmt <= '9')
      sp.width = sp.width * 10 + (*fmt++ - '0');

    int is_long = 0; /* long and long long are both 64-bit here */
    while (*fmt == 'l') {
      is_long = 1;
      fmt++;
    }

    switch (*fmt) {
    case 'd':
    case 'i': {
      long v = is_long ? va_arg(args, long) : va_arg(args, int);
      /* negate in unsigned so LONG_MIN doesn't overflow */
      unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
      put_uint(&s, u, 10, v < 0, "", &sp);
      break;
    }
    case 'u':
    case 'x': {
      unsigned long v = is_long ? va_arg(args, unsigned long)
                                : va_arg(args, unsigned int);
      put_uint(&s, v, *fmt == 'u' ? 10 : 16, 0, "", &sp);
      break;
    }
    case 'p':
      put_uint(&s, (uintptr_t)va_arg(args, void *), 16, 0, "0x", &sp);
      break;
    case 's':
      put_str(&s, va_arg(args, const char *), &sp);
      break;
    case 'c':
      sink_putc(&s, (char)va_arg(args, int)); /* char is promoted to int */
      break;
    case '%':
      sink_putc(&s, '%');
      break;
    case '\0':
      /* trailing lone "%...": emit it as-is, don't read past the end */
      while (start < fmt)
        sink_putc(&s, *start++);
      continue;
    default:
      /* unknown conversion: emit it literally so the bug is visible */
      while (start <= fmt)
        sink_putc(&s, *start++);
      break;
    }
    fmt++;
  }

  if (buf_size > 0)
    buf[s.pos < buf_size ? s.pos : buf_size - 1] = '\0';
  return (int)s.pos;
}

int ksnprintf(char *buf, size_t buf_size, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int r = kvsnprintf(buf, buf_size, fmt, args);
  va_end(args);
  return r;
}
