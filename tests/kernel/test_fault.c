#ifdef WARDEN_TEST
#include "ktest.h"
#include "cpu.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"

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

int test_wx_text(void) {
    probe_begin();
    void *text = (void *)test_wx_text;
    int faulted = write_faults(text);
    probe_end();
    KASSERT(faulted);
    KASSERT(fault_cr2 == (uint64_t)text);
    KASSERT((fault_err & (PF_P | PF_W)) == (PF_P | PF_W));
    return 0;
}

int test_wx_rodata(void) {
    probe_begin();
    int faulted = write_faults((volatile void *)&some_rodata);
    probe_end();
    KASSERT(faulted);
    KASSERT(fault_cr2 == (uint64_t)&some_rodata);
    KASSERT(some_rodata == 7);
    return 0;
}

int test_wx_data_noexec(void) {
    probe_begin();
    fault_ret = 1; /* the fault happens on the call target, not mid-asm */
    ((void (*)(void))some_data)();
    probe_end();
    KASSERT(fault_cr2 == (uint64_t)some_data);
    KASSERT((fault_err & (PF_P | PF_I)) == (PF_P | PF_I));
    return 0;
}

int test_unmapped_faults(void) {
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

#endif /* ifdef WARDEN_TEST */
