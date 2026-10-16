#ifdef WARDEN_TEST
#include "ktest.h"
#include "pmm.h"
#include "sched.h"
#include "slab.h"
#include "timer.h"

/* Wait (bounded) until only the test thread is left. */
static int wait_threads_done(void) {
    for (int spin = 0; sched_nthreads() > 1 && spin < 1000000; spin++)
        thread_yield();
    return sched_nthreads() == 1;
}

static volatile int spin_stop;
static volatile uint64_t spin_count[2];

static void spinner(void *arg) {
    int i = (int)(uintptr_t)arg;
    while (!spin_stop)
        spin_count[i]++;
}

/* Two threads that never yield both make progress: the timer preempts. */
int test_sched_preempt(void) {
    spin_stop = 0;
    spin_count[0] = spin_count[1] = 0;
    KASSERT(thread_create("spin0", spinner, (void *)0) != NULL);
    KASSERT(thread_create("spin1", spinner, (void *)1) != NULL);
    uint64_t start = timer_ticks();
    for (int spin = 0; timer_ticks() < start + 4 * SCHED_QUANTUM &&
                       spin < 1000; spin++)
        __asm__ __volatile__("hlt");
    spin_stop = 1;
    KASSERT(wait_threads_done());
    KASSERT(spin_count[0] > 0 && spin_count[1] > 0);
    return 0;
}

static char order[16];
static volatile int norder;

static void appender(void *arg) {
    for (int i = 0; i < 3; i++) {
        order[norder++] = *(const char *)arg;
        thread_yield();
    }
}

/* Yield hands the CPU to the next thread in creation order. */
int test_sched_round_robin(void) {
    norder = 0;
    KASSERT(thread_create("A", appender, "A") != NULL);
    KASSERT(thread_create("B", appender, "B") != NULL);
    KASSERT(thread_create("C", appender, "C") != NULL);
    KASSERT(wait_threads_done());
    order[norder] = 0;
    KASSERT(streq(order, "ABCABCABC"));
    return 0;
}

static void nop_thread(void *arg) { (void)arg; }

/* An exited thread's stack and struct go back to the allocator. */
int test_sched_reap(void) {
    sched_reap();
    kmalloc_shrink();
    size_t before = pmm_free_count();
    for (int i = 0; i < 8; i++)
        KASSERT(thread_create("nop", nop_thread, NULL) != NULL);
    KASSERT(wait_threads_done());
    sched_reap();
    kmalloc_shrink();
    KASSERT(pmm_free_count() == before);
    return 0;
}

static volatile int churn_stop;
static volatile int churn_bad;

/* Allocate and free across the size classes and the large (page) path,
 * checking our own bytes survive, until told to stop. */
static void churner(void *arg) {
    uint8_t tag = (uint8_t)(uintptr_t)arg;
    static const size_t sizes[] = {16, 48, 200, 1000, 2048, 5000, 9000};
    void *p[7];
    while (!churn_stop) {
        for (int i = 0; i < 7; i++) {
            p[i] = kmalloc(sizes[i]);
            if (p[i] == NULL) {
                churn_bad = 1;
                return;
            }
            uint8_t *q = p[i];
            q[0] = q[sizes[i] - 1] = tag; /* ends only: keep the loop in kmalloc */
        }
        for (int i = 0; i < 7; i++) {
            uint8_t *q = p[i];
            if (q[0] != tag || q[sizes[i] - 1] != tag)
                churn_bad = 1;
            kfree(q);
        }
    }
}

/* Threads preempted mid-kmalloc don't corrupt the allocators: every byte
 * stays ours, and every page comes back. Probabilistic: with the locks
 * stubbed out this failed about 1 run in 3, so a pass is weak evidence. */
int test_alloc_concurrent(void) {
    sched_reap();
    kmalloc_shrink();
    size_t before = pmm_free_count();
    churn_stop = 0;
    churn_bad = 0;
    for (uintptr_t i = 1; i <= 3; i++)
        KASSERT(thread_create("churn", churner, (void *)i) != NULL);
    uint64_t start = timer_ticks();
    for (int spin = 0; timer_ticks() < start + 40 * SCHED_QUANTUM &&
                       spin < 1000; spin++)
        __asm__ __volatile__("hlt");
    churn_stop = 1;
    KASSERT(wait_threads_done());
    KASSERT(!churn_bad);
    sched_reap();
    kmalloc_shrink();
    KASSERT(pmm_free_count() == before);
    return 0;
}

#endif /* ifdef WARDEN_TEST */
