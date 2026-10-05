#include <io.h>
#include <limine.h>
#include <serial.h>
#include <stdbool.h>

#include <ktest.h>

/** Limine bootloader requests */

__attribute((used, section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_ends[] = LIMINE_REQUESTS_END_MARKER;

static __attribute__((noreturn)) void halt(void) {
#ifdef WARDEN_TEST
  ktest_fail();
#endif /* ifdef WARDEN_TEST */
  __asm__ __volatile__("hlt");
  while (1) {
    __asm__ __volatile__("hlt");
  }
}

int kmain() {

  serial_init(COM1);

  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    serial_puts(COM1, "Unsupported Limine base revision\n");
    halt();
  }

  serial_puts(COM1, "warden: hello, World!\n");
  serial_puts(COM1, "warden: BOOT ok\n");


#ifdef WARDEN_TEST
  ktest_run_all();
#endif /* ifdef WARDEN_TEST */
  halt();
}
