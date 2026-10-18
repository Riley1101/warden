#include <apic.h>
#include <cpu.h>
#include <gdt.h>
#include <idt.h>
#include <kprintf.h>
#include <limine.h>
#include <pmm.h>
#include <fs.h>
#include <sched.h>
#include <serial.h>
#include <slab.h>
#include <syscall.h>
#include <timer.h>
#include <vmm.h>

#include <ktest.h>

/** Limine bootloader requests */

__attribute((used, section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute((used, section(".limine_requests"))) static volatile struct
    limine_executable_address_request kaddr_request = {
        .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0};

__attribute((
    used,
    section(".limine_requests"))) static volatile struct limine_module_request
    module_request = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

__attribute((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_ends[] = LIMINE_REQUESTS_END_MARKER;

/* Bootloader-reclaimable memory: Limine's page tables, its stack (which we
 * boot on), and the memmap itself. Freed once we no longer use any of it. */
static struct pmm_region reclaimable[PMM_MAX_REGIONS];

static size_t nreclaimable;

/* Memory types that get an HHDM mapping in our page tables. */
static int hhdm_mapped(uint64_t type) {
  switch (type) {
  case LIMINE_MEMMAP_USABLE:
  case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
  case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
  case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
  case LIMINE_MEMMAP_ACPI_NVS:
  case LIMINE_MEMMAP_FRAMEBUFFER:
  case LIMINE_MEMMAP_RESERVED_MAPPED:
    return 1;
  default:
    return 0; /* reserved, bad memory */
  }
}

/**
 * Set up the PMM and VMM. The PMM is initialized with usable memory, and the VMM
 * is initialized with the mapped memory regions, the physical base of the kernel,
 */
static void memory_setup(void) {
  struct limine_memmap_response *mm = memmap_request.response;
  struct limine_hhdm_response *hhdm = hhdm_request.response;
  struct limine_executable_address_response *kaddr = kaddr_request.response;
  if (mm == NULL || hhdm == NULL || kaddr == NULL)
    panic("Limine did not provide memmap / HHDM / executable address");

  struct pmm_region usable[PMM_MAX_REGIONS];
  struct pmm_region mapped[PMM_MAX_REGIONS];
  size_t nusable = 0, nmapped = 0;
  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];
    struct pmm_region r = {e->base, e->length};
    if (nusable == PMM_MAX_REGIONS || nmapped == PMM_MAX_REGIONS ||
        nreclaimable == PMM_MAX_REGIONS)
      panic("memmap: more than %d entries of one kind", PMM_MAX_REGIONS);
    if (e->type == LIMINE_MEMMAP_USABLE)
      usable[nusable++] = r;
    if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
      reclaimable[nreclaimable++] = r;
    if (hhdm_mapped(e->type))
      mapped[nmapped++] = r;
  }

  /**
   * Initialize the PMM with usable memory. The PMM will manage physical memory
   * and provide frames for the VMM to map. The HHDM offset is used to translate
   * physical addresses to virtual addresses in the higher half.
   */
  /* Size the bitmap to also cover the reclaimable memory we free later. */
  uint64_t top = 0;
  for (size_t i = 0; i < nreclaimable; i++)
    if (reclaimable[i].base + reclaimable[i].len > top)
      top = reclaimable[i].base + reclaimable[i].len;

  int err = pmm_init(usable, nusable, top, hhdm->offset);
  if (err != 0)
    panic("pmm_init failed: %d", err);

  /**
   * Initialize the VMM with the mapped memory regions, the physical base of the
   * kernel, and the virtual base of the kernel. The VMM will set up page tables
   * and switch to the new address space.
   */
  vmm_init(mapped, nmapped, kaddr->physical_base, kaddr->virtual_base);
  kmalloc_init();
}

/* The initramfs is the one module (boot/limine.conf): unpacked into the
 * filesystem now, while the response describing it is still ours (it is
 * in bootloader-reclaimable memory). */
static void initrd_setup(void) {
  if (fs_init() != 0)
    panic("fs: no memory for the root");
  struct limine_module_response *r = module_request.response;
  if (r == NULL || r->module_count == 0) {
    kprintf("initrd: none, the filesystem is empty\n");
    return;
  }
  struct limine_file *f = r->modules[0];
  int err = fs_load_tar(f->address, f->size);
  if (err != 0)
    panic("initrd: unpacking failed: %d", err);
  kprintf("initrd: %lu KiB unpacked\n", f->size / 1024);
}

/* Once we're off Limine's stack and page tables, its memory is ours. */
static void reclaim_bootloader_memory(void) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < nreclaimable; i++) {
    int err = pmm_add_region(&reclaimable[i]);
    if (err == 0)
      bytes += reclaimable[i].len;
    else
      kprintf("pmm: skipped reclaimable %lx+%lx: %d\n", reclaimable[i].base,
              reclaimable[i].len, err);
  }
  kprintf("pmm: reclaimed %lu KiB from the bootloader\n", bytes / 1024);
}

static void cpu_setup(void) {
  /* No floating point anywhere: the kernel is built without it, and threads
   * don't save x87/SSE state, so in ring 3 it would leak between processes.
   * With EM set, x87 traps (#NM) and SSE is undefined (#UD). */
  write_cr0(read_cr0() | CR0_EM);

  /* The kernel touches user memory only through usercopy: a stray user
   * pointer, or a jump into user code, faults instead of working. */
  uint32_t a, b, c, d;
  cpuid(0, &a, &b, &c, &d);
  uint32_t max_leaf = a;
  b = 0;
  if (max_leaf >= 7)
    cpuid(7, &a, &b, &c, &d);
  if (!(b & CPUID7_SMEP) || !(b & CPUID7_SMAP))
    panic("cpu: SMEP and SMAP are required (QEMU: -cpu max)");
  write_cr4(read_cr4() | CR4_SMEP | CR4_SMAP);
}

#define BOOT_STACK_SIZE (64 * 1024)
static uint8_t boot_stack[BOOT_STACK_SIZE] __attribute__((aligned(16)));

/* Runs on boot_stack, in our own address space. */
static __attribute__((noreturn)) void kmain_late(void) {
  /* From here on, Limine's responses point at freed memory: don't use them. */
  reclaim_bootloader_memory();
  kprintf("pmm: %lu MiB free\n", pmm_free_count() * PAGE_SIZE / (1024 * 1024));

  kprintf("warden: hello, World!\n");
  kprintf("warden: BOOT ok\n");

  apic_init();
  sched_init();
  timer_init();
  __asm__ __volatile__("sti");

#ifdef WARDEN_TEST
  ktest_run_all();
#endif /* ifdef WARDEN_TEST */
  /* nothing to run yet: idle forever */
  while (1) {
    __asm__ __volatile__("hlt");
  }
}

int kmain() {

  serial_init(COM1);

  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    panic("unsupported Limine base revision");
  }

  cpu_setup();
  gdt_init();
  idt_init();
  syscall_init();
  memory_setup();
  initrd_setup();

  /* Leave Limine's stack (bootloader-reclaimable memory) for our own. A call,
   * not a jmp, so kmain_late starts with the stack aligned as the ABI wants. */
  __asm__ __volatile__("mov %0, %%rsp\n\t"
                       "xor %%ebp, %%ebp\n\t"
                       "call *%1"
                       :
                       : "r"(boot_stack + sizeof boot_stack), "r"(kmain_late)
                       : "memory");
  __builtin_unreachable();
}
