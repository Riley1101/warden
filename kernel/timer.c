#include "timer.h"

#include <apic.h>
#include <idt.h>
#include <kprintf.h>
#include <sched.h>

static volatile uint64_t ticks;

static void on_tick(struct trap_frame *f) {
  (void)f;
  ticks++;
  apic_eoi(); /* before sched_tick: it may switch away for a whole quantum */
  sched_tick();
}

void timer_init(void) {
  uint32_t per_ms = apic_timer_calibrate();
  idt_register(T_TIMER, on_tick);
  apic_timer_periodic(T_TIMER, per_ms * (1000 / TIMER_HZ));
  kprintf("timer: %u lapic ticks/ms, %u Hz\n", per_ms, TIMER_HZ);
}

uint64_t timer_ticks(void) { return ticks; }
