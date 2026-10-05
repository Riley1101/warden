#ifdef WARDEN_TEST

#include <ktest.h>

#include <io.h>
#include <serial.h>

#define QEMU_EXIT_PORT 0xf4
#define QEMU_EXIT_PASS 0x10
#define QEMU_EXIT_FAIL 0x11

static __attribute__((noreturn)) void qemu_exit(uint8_t code) {
  outb(QEMU_EXIT_PORT, code);
  while (1) {
    __asm__ __volatile__("hlt");
  }
}

void ktest_fail(void) { qemu_exit(QEMU_EXIT_FAIL); }

extern const struct ktest ktests[];
extern const int ktests_count;

void ktest_run_all(void) {
  int failed = 0;
  serial_puts(COM1, "Running kernel tests...\n");
  for (int i = 0; i < ktests_count; i++) {
    serial_puts(COM1, "  - ");
    serial_puts(COM1, ktests[i].name);
    serial_puts(COM1, "\n");
    if (ktests[i].fn()) {
      failed = 1;
    }
  }
  serial_puts(COM1, failed ? "ktest: FAILED!\n" : "ktest: PASSED!\n");

  qemu_exit(failed ? QEMU_EXIT_FAIL : QEMU_EXIT_PASS);
}

#endif /* ifdef WARDEN_TEST */
