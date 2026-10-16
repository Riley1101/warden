#pragma once

#include <stdint.h>

/* Local APIC. Kernel only; one CPU for now. */

#define T_APIC_SPURIOUS 0xFF

/*
 * Mask the legacy 8259 PIC, map the local APIC registers (uncached) and
 * enable it, and install the spurious-interrupt handler. Call after
 * vmm_init() and idt_init(), before enabling interrupts. Panics on failure.
 */
void apic_init(void);

/* Signal end of interrupt. Every APIC-delivered handler must call this,
 * except the spurious one. */
void apic_eoi(void);

/* This CPU's local APIC ID. */
uint32_t apic_id(void);

/* Send `vector` to this CPU as a fixed interrupt. */
void apic_self_ipi(uint8_t vector);

/*
 * Measure the LAPIC timer against the PIT. Returns timer ticks per
 * millisecond at divide-by-16. Busy-waits ~10 ms; leaves the timer stopped.
 */
uint32_t apic_timer_calibrate(void);

/* Fire `vector` every `count` timer ticks (divide-by-16), forever. */
void apic_timer_periodic(uint8_t vector, uint32_t count);
