#include <gdt.h>
#include <kprintf.h>
#include <limine.h>
#include <pmm.h>
#include <serial.h>
#include <idt.h>

#include <ktest.h>

/** Limine bootloader requests */

__attribute((used, section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute((used, section(".limine_requests"))) static volatile struct
    limine_memmap_request memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID,
                                            .revision = 0};

__attribute((used, section(".limine_requests"))) static volatile struct
    limine_hhdm_request hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID,
                                        .revision = 0};

__attribute((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_ends[] = LIMINE_REQUESTS_END_MARKER;

/* Hand Limine's usable memory to the PMM. Bootloader-reclaimable memory
 * (Limine's page tables, this very memmap) stays reserved until we run on
 * our own page tables. */
static void pmm_setup(void) {
  struct limine_memmap_response *mm = memmap_request.response;
  struct limine_hhdm_response *hhdm = hhdm_request.response;
  if (mm == NULL || hhdm == NULL)
    panic("Limine did not provide a memory map / HHDM");

  struct pmm_region usable[PMM_MAX_REGIONS];
  size_t n = 0;
  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];
    if (e->type != LIMINE_MEMMAP_USABLE)
      continue;
    if (n == PMM_MAX_REGIONS)
      panic("pmm: more than %d usable regions", PMM_MAX_REGIONS);
    usable[n++] = (struct pmm_region){e->base, e->length};
  }

  int err = pmm_init(usable, n, hhdm->offset);
  if (err != 0)
    panic("pmm_init failed: %d", err);
  kprintf("pmm: %lu MiB free in %lu regions\n",
          pmm_free_count() * PAGE_SIZE / (1024 * 1024), n);
}

int kmain() {

  serial_init(COM1);

  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    panic("unsupported Limine base revision");
  }

  gdt_init();
  idt_init();
  pmm_setup();

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
