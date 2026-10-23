#include "apic.h"

#include <abi/errno.h>
#include <cpu.h>
#include <idt.h>
#include <io.h>
#include <kprintf.h>
#include <pmm.h>
#include <pt.h>
#include <vmm.h>

/**
 * Local APIC: memory-mapped, 32-bit registers on 16-byte boundaries.
 * @see https://wiki.osdev.org/APIC
 */
#define MSR_APIC_BASE      0x1B
#define APIC_BASE_ENABLE   (1UL << 11) /* global enable */
#define APIC_BASE_ADDR     0x000FFFFFFFFFF000UL

#define CPUID_1_EDX_APIC   (1U << 9)

#define LAPIC_ID       0x20
#define LAPIC_VERSION  0x30
#define LAPIC_TPR      0x80
#define LAPIC_EOI      0xB0
#define LAPIC_SVR      0xF0
#define LAPIC_ICR_LO   0x300
#define LAPIC_ICR_HI   0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define SVR_ENABLE     (1U << 8)  /* software enable */
#define ICR_SELF       (1U << 18) /* destination shorthand 01: self */
#define ICR_PENDING    (1U << 12) /* delivery status: send pending */
#define LVT_MASKED     (1U << 16)
#define LVT_PERIODIC   (1U << 17)
#define TIMER_DIV_16   0x3

/* PIT channel 2: its gate and output are wired to port 0x61, so we can
 * run it and poll it without taking an interrupt. */
#define PIT_CH2      0x42
#define PIT_CMD      0x43
#define PIT_PORT_B   0x61
#define PORTB_GATE2  (1U << 0) /* channel 2 gate: counts while high */
#define PORTB_SPKR   (1U << 1) /* speaker data: keep it off */
#define PORTB_OUT2   (1U << 5) /* channel 2 output (read only) */
#define PIT_HZ       1193182
#define CALIBRATE_MS 10

/*
 * I/O APIC: the legacy ISA interrupts (the UARTs, among others) arrive
 * here, not at the local APIC. Two 32-bit windows: write a register
 * number to IOREGSEL, read or write it through IOWIN. Its address is the
 * PC-standard one, which is what QEMU's q35 uses; a machine that moved it
 * would need its MADT read.
 */
#define IOAPIC_PHYS   0xFEC00000UL
#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10
#define IOAPIC_REDTBL(gsi) (0x10 + 2 * (uint32_t)(gsi))
#define REDIR_MASKED  (1U << 16)

/* 8259 PIC data ports (interrupt mask registers) */
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

static volatile uint8_t *lapic;
static volatile uint8_t *ioapic;

static uint32_t lapic_read(uint32_t reg) {
  return *(volatile uint32_t *)(lapic + reg);
}

static void lapic_write(uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)(lapic + reg) = val;
}

static uint32_t ioapic_read(uint32_t reg) {
  *(volatile uint32_t *)(ioapic + IOAPIC_REGSEL) = reg;
  return *(volatile uint32_t *)(ioapic + IOAPIC_WIN);
}

static void ioapic_write(uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)(ioapic + IOAPIC_REGSEL) = reg;
  *(volatile uint32_t *)(ioapic + IOAPIC_WIN) = val;
}

/* Spurious interrupts set no in-service bit: an EOI here would acknowledge
 * some other, real interrupt. */
static void on_spurious(struct trap_frame *f) { (void)f; }

void apic_init(void) {
  uint32_t a, b, c, d;
  cpuid(1, &a, &b, &c, &d);
  if (!(d & CPUID_1_EDX_APIC))
    panic("apic: no local APIC");

  /* We never use the 8259, but unmasked it would deliver IRQ0 on vector 8,
   * which is #DF. Masking every line is enough; no need to remap it. */
  outb(PIC1_DATA, 0xFF);
  outb(PIC2_DATA, 0xFF);

  uint64_t msr = rdmsr(MSR_APIC_BASE);
  if (!(msr & APIC_BASE_ENABLE))
    wrmsr(MSR_APIC_BASE, msr | APIC_BASE_ENABLE);
  uint64_t phys = msr & APIC_BASE_ADDR;

  /* MMIO is not in the memmap, so the HHDM doesn't cover it. Map it at its
   * HHDM address anyway, uncached: a cached read returns stale registers. */
  uint64_t virt = (uint64_t)phys_to_virt(phys);
  int err = vmm_map(virt, phys, PTE_W | PTE_PCD | PTE_PWT | PTE_NX);
  if (err != 0 && err != -EEXIST)
    panic("apic: map %lx failed: %d", phys, err);
  lapic = (volatile uint8_t *)virt;

  idt_register(T_APIC_SPURIOUS, on_spurious);
  lapic_write(LAPIC_SVR, SVR_ENABLE | T_APIC_SPURIOUS);
  lapic_write(LAPIC_TPR, 0); /* accept every priority class */

  kprintf("lapic: id=%u ver=0x%x base=%lx\n", apic_id(),
          lapic_read(LAPIC_VERSION) & 0xFF, phys);

  virt = (uint64_t)phys_to_virt(IOAPIC_PHYS);
  err = vmm_map(virt, IOAPIC_PHYS, PTE_W | PTE_PCD | PTE_PWT | PTE_NX);
  if (err != 0 && err != -EEXIST)
    panic("ioapic: map %lx failed: %d", IOAPIC_PHYS, err);
  ioapic = (volatile uint8_t *)virt;
}

void apic_route_irq(uint8_t gsi, uint8_t vector) {
  /* High half: which CPU. Low half: the vector, delivered as a fixed
   * interrupt, active high and edge triggered (the ISA default), with the
   * mask bit cleared last. */
  ioapic_write(IOAPIC_REDTBL(gsi) + 1, apic_id() << 24);
  ioapic_write(IOAPIC_REDTBL(gsi), vector);
}

void apic_mask_irq(uint8_t gsi) {
  ioapic_write(IOAPIC_REDTBL(gsi),
               ioapic_read(IOAPIC_REDTBL(gsi)) | REDIR_MASKED);
}

void apic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

uint32_t apic_id(void) { return lapic_read(LAPIC_ID) >> 24; }

void apic_self_ipi(uint8_t vector) {
  lapic_write(LAPIC_ICR_HI, 0);
  lapic_write(LAPIC_ICR_LO, ICR_SELF | vector); /* fixed delivery mode */
  while (lapic_read(LAPIC_ICR_LO) & ICR_PENDING)
    ;
}

uint32_t apic_timer_calibrate(void) {
  uint8_t portb = inb(PIT_PORT_B) & ~(PORTB_GATE2 | PORTB_SPKR);
  outb(PIT_PORT_B, portb); /* gate low: hold the PIT while we load it */

  /* channel 2, lobyte/hibyte, mode 0 (OUT goes high at terminal count) */
  uint16_t count = PIT_HZ * CALIBRATE_MS / 1000;
  outb(PIT_CMD, 0xB0);
  outb(PIT_CH2, count & 0xFF);
  outb(PIT_CH2, count >> 8);

  lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
  lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
  lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
  outb(PIT_PORT_B, portb | PORTB_GATE2); /* start both */
  while (!(inb(PIT_PORT_B) & PORTB_OUT2))
    ;
  uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
  lapic_write(LAPIC_TIMER_INIT, 0); /* stop */
  outb(PIT_PORT_B, portb);

  uint32_t per_ms = elapsed / CALIBRATE_MS;
  if (per_ms == 0)
    panic("apic: timer calibration failed");
  return per_ms;
}

void apic_timer_periodic(uint8_t vector, uint32_t count) {
  lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
  lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | vector);
  lapic_write(LAPIC_TIMER_INIT, count); /* writing the count starts it */
}
