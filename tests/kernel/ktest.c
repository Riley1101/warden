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

/* Every test, in the order they run. Each lives in tests/kernel/test_*.c
 * as `int test_<name>(void)`. */
#define KTESTS(X)                                                              \
  X(sanity)                                                                    \
  X(path_in_kernel)                                                            \
  X(fmt_in_kernel)                                                             \
  X(idt_breakpoint)                                                            \
  X(idt_frame)                                                                 \
  X(pmm_real_memory)                                                           \
  X(vmm_active)                                                                \
  X(pmm_all_frames)                                                            \
  X(vmm_map)                                                                   \
  X(kmalloc_kernel)                                                            \
  X(kmalloc_churn)                                                             \
  X(wx_text)                                                                   \
  X(wx_rodata)                                                                 \
  X(wx_data_noexec)                                                            \
  X(unmapped_faults)                                                           \
  X(apic_self_ipi)                                                             \
  X(timer_ticks)                                                               \
  X(sched_preempt)                                                             \
  X(sched_round_robin)                                                         \
  X(sched_reap)                                                                \
  X(alloc_concurrent)

#define KTEST_DECL(name) int test_##name(void);
#define KTEST_ENTRY(name) {#name, test_##name},

KTESTS(KTEST_DECL)

static const struct ktest ktests[] = {KTESTS(KTEST_ENTRY)};
static const int ktests_count = sizeof(ktests) / sizeof(ktests[0]);

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
