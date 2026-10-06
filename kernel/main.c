#include <gdt.h>
#include <kprintf.h>
#include <limine.h>
#include <serial.h>
#include <idt.h>

#include <ktest.h>

/** Limine bootloader requests */

__attribute((used, section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_ends[] = LIMINE_REQUESTS_END_MARKER;

int kmain() {

  serial_init(COM1);

  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    panic("unsupported Limine base revision");
  }

  gdt_init();
  idt_init();

  kprintf("warden: hello, World!\n");
  kprintf("warden: BOOT ok\n");


#ifdef WARDEN_TEST
  ktest_run_all();
#endif /* ifdef WARDEN_TEST */
  /* nothing to run yet: idle forever */
  while (1) {
    __asm__ __volatile__("hlt");
  }
}
