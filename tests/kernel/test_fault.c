#ifdef WARDEN_TEST
#include "ktest.h"
#include "cpu.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "kstring.h"
#include "usercopy.h"

#include <abi/errno.h>

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

/* ---- the kernel and user memory ------------------------------------------ */

/*
 * A user address space with one read-only page holding `ret` and one
 * writable page, loaded in CR3. Interrupts stay off until uspace_leave so
 * the scheduler can't switch CR3 back under us.
 */
#define U_RO 0x400000UL
#define U_RW 0x401000UL

struct uspace {
    uint64_t pml4, ro, rw, flags;
};

static int uspace_enter(struct uspace *u) {
    u->pml4 = vmm_space_new();
    u->ro = pmm_alloc();
    u->rw = pmm_alloc();
    if (u->pml4 == 0 || u->ro == 0 || u->rw == 0)
        return -1;
    memset(phys_to_virt(u->ro), 0xC3, PAGE_SIZE); /* `ret` */
    memset(phys_to_virt(u->rw), 0, PAGE_SIZE);
    if (vmm_space_map(u->pml4, U_RO, u->ro, 0) != 0 ||
        vmm_space_map(u->pml4, U_RW, u->rw, PTE_W | PTE_NX) != 0)
        return -1;
    u->flags = irq_save();
    vmm_switch(u->pml4);
    return 0;
}

static void uspace_leave(struct uspace *u) {
    vmm_switch(vmm_kernel_pml4());
    vmm_space_free(u->pml4); /* and both frames */
    irq_restore(u->flags);
}

int test_usercopy_kernel(void) {
    struct uspace u;
    KASSERT(uspace_enter(&u) == 0);
    char out[4] = "abc";
    int to = copy_to_user((void *)U_RW, "xyz", 4);
    int from = copy_from_user(out, (const void *)U_RW, 4);
    int to_ro = copy_to_user((void *)U_RO, "xyz", 4);
    int from_unmapped = copy_from_user(out, (const void *)(U_RW + PAGE_SIZE), 1);
    int seen = memcmp(phys_to_virt(u.rw), "xyz", 4);
    /* refused by the range check, before a single byte moves */
    char untouched[4] = "abc";
    int wraps = copy_from_user(untouched, (const void *)U_RW, SIZE_MAX);
    int past_top = copy_from_user(untouched, (const void *)(USER_TOP - 1), 2);
    uspace_leave(&u);

    KASSERT(wraps == -EFAULT);
    KASSERT(past_top == -EFAULT);
    KASSERT(memcmp(untouched, "abc", 4) == 0);

    KASSERT(to == 0);
    KASSERT(from == 0);
    KASSERT(memcmp(out, "xyz", 4) == 0);
    KASSERT(seen == 0);
    KASSERT(to_ro == -EFAULT);
    KASSERT(from_unmapped == -EFAULT);
    return 0;
}

/* Outside usercopy, a kernel read of a user page faults (SMAP)... */
int test_smap_blocks_user_read(void) {
    struct uspace u;
    KASSERT(uspace_enter(&u) == 0);
    probe_begin();
    int faulted = read_faults((volatile void *)U_RW);
    probe_end();
    uspace_leave(&u);
    KASSERT(faulted);
    KASSERT(fault_cr2 == U_RW);
    KASSERT(fault_err & PF_P);
    return 0;
}

/* ...and so does running user code in ring 0 (SMEP). */
int test_smep_blocks_user_exec(void) {
    struct uspace u;
    KASSERT(uspace_enter(&u) == 0);
    probe_begin();
    fault_ret = 1;
    ((void (*)(void))U_RO)();
    probe_end();
    uspace_leave(&u);
    KASSERT(fault_cr2 == U_RO);
    KASSERT((fault_err & (PF_P | PF_I)) == (PF_P | PF_I));
    return 0;
}

#endif /* ifdef WARDEN_TEST */
