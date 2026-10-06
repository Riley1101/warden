#include "kprintf.h"

#include <fmt.h>
#include <ktest.h>
#include <serial.h>

#define KPRINTF_BUF 256

void kprintf(const char *fmt, ...) {
  char buf[KPRINTF_BUF];
  va_list args;
  va_start(args, fmt);
  kvsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  serial_puts(COM1, buf);
}

void panic(const char *fmt, ...) {
  char buf[KPRINTF_BUF];
  va_list args;
  va_start(args, fmt);
  kvsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);

  serial_puts(COM1, "PANIC: ");
  serial_puts(COM1, buf);
  serial_puts(COM1, "\n");

#ifdef WARDEN_TEST
  ktest_fail();
#endif /* ifdef WARDEN_TEST */
  __asm__ __volatile__("cli");
  while (1) {
    __asm__ __volatile__("hlt");
  }
}
