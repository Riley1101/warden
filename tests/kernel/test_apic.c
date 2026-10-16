#ifdef WARDEN_TEST
#include "ktest.h"
#include "apic.h"
#include "idt.h"
#include "timer.h"

#define T_IPI_TEST 0x41

static volatile int ipi_hits;

static void on_ipi_test(struct trap_frame *f) {
    (void)f;
    ipi_hits++;
    apic_eoi();
}

/* A self-IPI arrives: LAPIC mapped, IDT routing and EOI all work. Sent
 * twice, since a missing EOI would block the second one. */
int test_apic_self_ipi(void) {
    idt_register(T_IPI_TEST, on_ipi_test);
    ipi_hits = 0;
    for (int n = 1; n <= 2; n++) {
        apic_self_ipi(T_IPI_TEST);
        for (int spin = 0; ipi_hits < n && spin < 1000000; spin++)
            __asm__ __volatile__("pause");
        KASSERT(ipi_hits == n);
    }
    idt_register(T_IPI_TEST, 0);
    return 0;
}

/* The periodic timer fires and keeps firing: each tick needs an EOI for
 * the next one to arrive. Doesn't check the rate. */
int test_timer_ticks(void) {
    uint64_t start = timer_ticks();
    for (int spin = 0; timer_ticks() < start + 5 && spin < 1000; spin++)
        __asm__ __volatile__("hlt"); /* each tick wakes us */
    KASSERT(timer_ticks() >= start + 5);
    return 0;
}

#endif /* ifdef WARDEN_TEST */
