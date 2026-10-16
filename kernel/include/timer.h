#pragma once

#include <stdint.h>

/* The system tick: the LAPIC timer in periodic mode. */

#define T_TIMER  0x20
#define TIMER_HZ 100

/* Calibrate the LAPIC timer and start it at TIMER_HZ. Call after
 * apic_init(), before enabling interrupts. */
void timer_init(void);

/* Ticks since timer_init(). */
uint64_t timer_ticks(void);
