#ifdef WARDEN_TEST
#include "fmt.h"
#include <abi/errno.h>
#include "ktest.h"
#include "path.h"
#include "pmm.h"
#include "vmm.h"
#include "slab.h"
#include "kstring.h"
#include "cpu.h"
#include "idt.h"
#include "apic.h"
#include "timer.h"
#include "sched.h"

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int test_path_in_kernel(void) {
    char out[PATH_MAX];
    KASSERT(path_normalize("/home/../bin//ls", out, sizeof out) == 0);
    KASSERT(streq(out, "/bin/ls"));
    return 0;
}

static int test_fmt_in_kernel(void) {
    char out[64];
    KASSERT(ksnprintf(out, sizeof out, "%d %s %016lx", -7, "ok",
                      0xffffffff80000000UL) == 22);
    KASSERT(streq(out, "-7 ok ffffffff80000000"));
    return 0;
}

static int bp_hits;
static uint64_t bp_rip;

static void on_breakpoint(struct trap_frame *f) {
    bp_hits++;
    bp_rip = f->rip;
}

/* int3 is a trap: the handler runs and execution resumes after it. */
static int test_idt_breakpoint(void) {
    idt_register(T_BREAKPOINT, on_breakpoint);
    bp_hits = 0;
    __asm__ __volatile__("int3");
    __asm__ __volatile__("int3");
    idt_register(T_BREAKPOINT, 0);
    KASSERT(bp_hits == 2);
    KASSERT(bp_rip >= 0xffffffff80000000UL); 
    return 0;
}

#define T_TEST 0x40

static void on_test_vector(struct trap_frame *f) {
    /* check the frame layout: registers land where idt.asm says */
    if (f->vector == T_TEST && f->error == 0 && f->rbx == 0xb0b &&
        f->r12 == 0x1212)
        f->rax += 1; /* writes to the frame reach the registers on return */
}

static int test_idt_frame(void) {
    idt_register(T_TEST, on_test_vector);
    uint64_t rax = 0xcafe;
    register uint64_t r12 __asm__("r12") = 0x1212;
    __asm__ __volatile__("int %[v]"
                         : "+a"(rax)
                         : [v] "i"(T_TEST), "b"(0xb0bUL), "r"(r12)
                         : "memory");
    idt_register(T_TEST, 0);
    KASSERT(rax == 0xcafe + 1);
    return 0;
}

/* The real memory map: frames come back usable through the HHDM and
 * every one of them is returned. */
static int test_pmm_real_memory(void) {
    size_t before = pmm_free_count();
    KASSERT(before > 0);

    uint64_t frames[16];
    for (int i = 0; i < 16; i++) {
        frames[i] = pmm_alloc();
        KASSERT(frames[i] != 0);
        KASSERT(frames[i] % PAGE_SIZE == 0);
        uint64_t *p = phys_to_virt(frames[i]);
        for (size_t j = 0; j < PAGE_SIZE / 8; j++)
            p[j] = frames[i] ^ j;
    }
    for (int i = 0; i < 16; i++) {
        uint64_t *p = phys_to_virt(frames[i]);
        for (size_t j = 0; j < PAGE_SIZE / 8; j++)
            KASSERT(p[j] == (frames[i] ^ j)); /* no two frames overlap */
    }
    KASSERT(pmm_free_count() == before - 16);

    for (int i = 0; i < 16; i++)
        KASSERT(pmm_free(frames[i]) == 0);
    KASSERT(pmm_free(frames[0]) != 0); /* double free is caught */
    KASSERT(pmm_free_count() == before);
    return 0;
}

static int test_vmm_active(void) {
    KASSERT(read_cr3() == vmm_kernel_pml4()); /* off Limine's tables */
    KASSERT(rdmsr(MSR_EFER) & EFER_NXE);
    KASSERT(read_cr0() & CR0_WP);
    return 0;
}

/* An address in the kernel half that nothing else uses. */
#define TEST_VIRT 0xffffc00000000000UL

static int test_vmm_map(void) {
    uint64_t frame = pmm_alloc();
    KASSERT(frame != 0);
    KASSERT(vmm_map(TEST_VIRT, frame, PTE_W | PTE_NX) == 0);
    KASSERT(vmm_map(TEST_VIRT, frame, PTE_W | PTE_NX) == -EEXIST);

    /* the same frame seen through two mappings */
    volatile uint64_t *v = (volatile uint64_t *)TEST_VIRT;
    volatile uint64_t *h = phys_to_virt(frame);
    *v = 0x1234567890abcdefUL;
    KASSERT(*h == 0x1234567890abcdefUL);
    *h = 42;
    KASSERT(*v == 42);

    KASSERT(vmm_unmap(TEST_VIRT) == frame);
    KASSERT(pmm_free(frame) == 0);
    return 0;
}

/*
 * Expected-fault probes. The #PF handler records the fault and resumes at
 * `fixup_rip`, which each probe points just past its faulting instruction.
 * (usercopy will need the same idea, as an exception table.)
 */
static volatile uint64_t fixup_rip;
static volatile uint64_t fault_cr2, fault_err;
static volatile int fault_ret; /* resume by emulating `ret` instead */

static void on_expected_pf(struct trap_frame *f) {
    fault_cr2 = read_cr2();
    fault_err = f->error;
    if (fault_ret) {
        f->rip = *(uint64_t *)f->rsp; /* the caller's return address */
        f->rsp += 8;
    } else {
        f->rip = fixup_rip;
    }
}

static void probe_begin(void) {
    fault_cr2 = fault_err = 0;
    fault_ret = 0;
    idt_register(T_PAGE_FAULT, on_expected_pf);
}

static void probe_end(void) { idt_register(T_PAGE_FAULT, 0); }

/* Returns 1 if writing to p faulted. */
static int write_faults(volatile void *p) {
    int faulted;
    __asm__ __volatile__("leaq 1f(%%rip), %%rax\n\t"
                         "movq %%rax, %[fix]\n\t"
                         "movl $1, %[out]\n\t"
                         "movb $0, (%[p])\n\t" /* may fault */
                         "movl $0, %[out]\n\t"
                         "1:"
                         : [out] "=&r"(faulted), [fix] "=m"(fixup_rip)
                         : [p] "r"(p)
                         : "rax", "memory");
    return faulted;
}

/* Returns 1 if reading from p faulted. */
static int read_faults(volatile void *p) {
    int faulted;
    __asm__ __volatile__("leaq 1f(%%rip), %%rax\n\t"
                         "movq %%rax, %[fix]\n\t"
                         "movl $1, %[out]\n\t"
                         "movb (%[p]), %%al\n\t" /* may fault */
                         "movl $0, %[out]\n\t"
                         "1:"
                         : [out] "=&r"(faulted), [fix] "=m"(fixup_rip)
                         : [p] "r"(p)
                         : "rax", "memory");
    return faulted;
}

#define PF_P 0x1  /* protection violation (vs not present) */
#define PF_W 0x2  /* write */
#define PF_I 0x10 /* instruction fetch */

static const uint64_t some_rodata = 7;
static uint8_t some_data[16] = {0xC3}; /* `ret` */

static int test_wx_text(void) {
    probe_begin();
    void *text = (void *)test_wx_text;
    int faulted = write_faults(text);
    probe_end();
    KASSERT(faulted);
    KASSERT(fault_cr2 == (uint64_t)text);
    KASSERT((fault_err & (PF_P | PF_W)) == (PF_P | PF_W));
    return 0;
}

static int test_wx_rodata(void) {
    probe_begin();
    int faulted = write_faults((volatile void *)&some_rodata);
    probe_end();
    KASSERT(faulted);
    KASSERT(fault_cr2 == (uint64_t)&some_rodata);
    KASSERT(some_rodata == 7);
    return 0;
}

static int test_wx_data_noexec(void) {
    probe_begin();
    fault_ret = 1; /* the fault happens on the call target, not mid-asm */
    ((void (*)(void))some_data)();
    probe_end();
    KASSERT(fault_cr2 == (uint64_t)some_data);
    KASSERT((fault_err & (PF_P | PF_I)) == (PF_P | PF_I));
    return 0;
}

static int test_unmapped_faults(void) {
    uint64_t frame = pmm_alloc();
    KASSERT(vmm_map(TEST_VIRT, frame, PTE_W | PTE_NX) == 0);
    probe_begin();
    int before = read_faults((volatile void *)TEST_VIRT);
    vmm_unmap(TEST_VIRT); /* must invalidate the cached translation */
    int after = read_faults((volatile void *)TEST_VIRT);
    probe_end();
    pmm_free(frame);
    KASSERT(!before);
    KASSERT(after);
    KASSERT(fault_cr2 == TEST_VIRT);
    KASSERT((fault_err & PF_P) == 0); /* not present */
    return 0;
}

/* Every free frame, reclaimed bootloader memory included, is reachable
 * and writable through our HHDM. Frames are chained through their first
 * word, so this needs no extra memory. */
static int test_pmm_all_frames(void) {
    size_t before = pmm_free_count();
    uint64_t head = 0;
    size_t n = 0;
    uint64_t f;
    while ((f = pmm_alloc()) != 0) {
        uint64_t *p = phys_to_virt(f);
        p[0] = head;
        p[PAGE_SIZE / 8 - 1] = ~f; /* touch both ends of the page */
        head = f;
        n++;
    }
    KASSERT(n == before);
    while (head != 0) {
        uint64_t *p = phys_to_virt(head);
        uint64_t next = p[0];
        KASSERT(p[PAGE_SIZE / 8 - 1] == ~head);
        KASSERT(pmm_free(head) == 0);
        head = next;
    }
    KASSERT(pmm_free_count() == before);
    return 0;
}

static int test_kmalloc_kernel(void) {
    size_t before = pmm_free_count();

    /* small: slab objects in the HHDM, usable and distinct */
    uint64_t *a = kmalloc(sizeof(uint64_t) * 4);
    uint64_t *b = kzalloc(100);
    KASSERT(a != NULL && b != NULL && a != (void *)b);
    KASSERT(virt_to_page(a) != NULL);
    KASSERT(virt_to_page(a)->flags & PG_SLAB);
    for (int i = 0; i < 4; i++)
        a[i] = 0xa0a0 + i;
    for (size_t i = 0; i < 100 / 8; i++)
        KASSERT(b[i] == 0);
    KASSERT(a[3] == 0xa0a3);

    /* large: contiguous pages */
    uint8_t *big = kmalloc(5 * PAGE_SIZE);
    KASSERT(big != NULL && ksize(big) == 5 * PAGE_SIZE);
    memset(big, 0x5c, 5 * PAGE_SIZE);
    KASSERT(big[5 * PAGE_SIZE - 1] == 0x5c);

    kfree(a);
    kfree(b);
    kfree(big);
    kmalloc_shrink();
    KASSERT(pmm_free_count() == before);
    return 0;
}

/* Many objects of every class at once, then all freed. */
static int test_kmalloc_churn(void) {
    size_t before = pmm_free_count();
    static void *p[1024];
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 1024; i++) {
            size_t size = (size_t)16 << (i % 8);
            p[i] = kmalloc(size);
            KASSERT(p[i] != NULL);
            memset(p[i], i & 0xff, size);
        }
        for (int i = 0; i < 1024; i++) {
            uint8_t *q = p[i];
            size_t size = (size_t)16 << (i % 8);
            KASSERT(q[0] == (i & 0xff) && q[size - 1] == (i & 0xff));
            kfree(q);
        }
    }
    kmalloc_shrink();
    KASSERT(pmm_free_count() == before);
    return 0;
}

#define T_IPI_TEST 0x41

static volatile int ipi_hits;

static void on_ipi_test(struct trap_frame *f) {
    (void)f;
    ipi_hits++;
    apic_eoi();
}

/* A self-IPI arrives: LAPIC mapped, IDT routing and EOI all work. Sent
 * twice, since a missing EOI would block the second one. */
static int test_apic_self_ipi(void) {
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
static int test_timer_ticks(void) {
    uint64_t start = timer_ticks();
    for (int spin = 0; timer_ticks() < start + 5 && spin < 1000; spin++)
        __asm__ __volatile__("hlt"); /* each tick wakes us */
    KASSERT(timer_ticks() >= start + 5);
    return 0;
}

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
static int test_sched_preempt(void) {
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
static int test_sched_round_robin(void) {
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
static int test_sched_reap(void) {
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

static int test_sanity(void) {
    KASSERT(1 + 1 == 2);
    KASSERT(2 * 2 == 4);
    return 0;
}

const struct ktest ktests[] = {
    {"sanity", test_sanity},
    {"path_in_kernel", test_path_in_kernel},
    {"fmt_in_kernel", test_fmt_in_kernel},
    {"idt_breakpoint", test_idt_breakpoint},
    {"idt_frame", test_idt_frame},
    {"pmm_real_memory", test_pmm_real_memory},
    {"vmm_active", test_vmm_active},
    {"pmm_all_frames", test_pmm_all_frames},
    {"vmm_map", test_vmm_map},
    {"kmalloc_kernel", test_kmalloc_kernel},
    {"kmalloc_churn", test_kmalloc_churn},
    {"wx_text", test_wx_text},
    {"wx_rodata", test_wx_rodata},
    {"wx_data_noexec", test_wx_data_noexec},
    {"unmapped_faults", test_unmapped_faults},
    {"apic_self_ipi", test_apic_self_ipi},
    {"timer_ticks", test_timer_ticks},
    {"sched_preempt", test_sched_preempt},
    {"sched_round_robin", test_sched_round_robin},
    {"sched_reap", test_sched_reap},
};
const int ktests_count = sizeof(ktests) / sizeof(ktests[0]);

#endif /* ifdef WARDEN_TEST */
