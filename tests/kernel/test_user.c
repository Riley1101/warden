#ifdef WARDEN_TEST
#include "ktest.h"
#include "kstring.h"
#include "pmm.h"
#include "sched.h"
#include "slab.h"
#include "timer.h"
#include "vmm.h"

#include <abi/errno.h>

/*
 * Ring 3 without syscalls: a few bytes of machine code in a user page.
 * Code at USER_CODE (read-only, executable), one stack page below
 * USER_STACK_TOP (writable, NX). The kernel watches through the HHDM.
 */
#define USER_CODE      0x400000UL
#define USER_STACK_TOP 0x800000UL
#define USER_STACK     (USER_STACK_TOP - PAGE_SIZE)

static uint64_t user_stack_frame;

static struct thread *run_user(const char *name, const uint8_t *code,
                               size_t len) {
    uint64_t pml4 = vmm_space_new();
    uint64_t text = pmm_alloc();
    uint64_t stack = pmm_alloc();
    if (pml4 == 0 || text == 0 || stack == 0)
        return NULL;
    memset(phys_to_virt(text), 0, PAGE_SIZE);
    memcpy(phys_to_virt(text), code, len);
    memset(phys_to_virt(stack), 0, PAGE_SIZE);
    if (vmm_space_map(pml4, USER_CODE, text, 0) != 0 ||
        vmm_space_map(pml4, USER_STACK, stack, PTE_W | PTE_NX) != 0)
        return NULL;
    user_stack_frame = stack;
    return thread_create_user(name, pml4, USER_CODE, USER_STACK_TOP);
}

static int wait_threads_done(void) {
    for (int spin = 0; sched_nthreads() > 1 && spin < 1000000; spin++)
        thread_yield();
    return sched_nthreads() == 1;
}

static void wait_ticks(uint64_t n) {
    uint64_t start = timer_ticks();
    for (int spin = 0; timer_ticks() < start + n && spin < 1000; spin++)
        __asm__ __volatile__("hlt");
}

/* Everything a dead user thread had (stack, struct, page tables, frames)
 * is back once it is reaped. */
static size_t free_frames(void) {
    sched_reap();
    kmalloc_shrink();
    return pmm_free_count();
}

/*   mov rax, USER_STACK ; 1: inc qword [rax] ; jmp 1b */
static const uint8_t prog_count[] = {
    0x48, 0xb8, 0x00, 0xf0, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xff, 0x00,
    0xeb, 0xfb,
};
_Static_assert(USER_STACK == 0x7ff000, "prog_count hardcodes USER_STACK");

/* A ring 3 loop runs, the timer still preempts it (we get the CPU back,
 * through rsp0), and killing it frees its whole address space. */
int test_user_preempt(void) {
    size_t before = free_frames();
    struct thread *t = run_user("count", prog_count, sizeof prog_count);
    KASSERT(t != NULL);
    volatile uint64_t *counter = phys_to_virt(user_stack_frame);

    wait_ticks(4 * SCHED_QUANTUM);
    uint64_t first = *counter;
    wait_ticks(4 * SCHED_QUANTUM);
    KASSERT(first > 0);
    KASSERT(*counter > first);

    thread_kill(t);
    KASSERT(wait_threads_done());
    KASSERT(free_frames() == before);
    return 0;
}

static const uint8_t prog_hlt[] = {0xf4}; /* privileged: #GP */
static const uint8_t prog_cli[] = {0xfa}; /* privileged: #GP */
static const uint8_t prog_int3[] = {0xcc}; /* gate is DPL 0: #GP */
/*   mov rax, 0xffffffff80000000 ; mov rax, [rax]   (kernel page: #PF) */
static const uint8_t prog_read_kernel[] = {
    0x48, 0xb8, 0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0xff,
    0x48, 0x8b, 0x00,
};
/*   mov rax, USER_CODE ; mov byte [rax], 0   (read-only page: #PF) */
static const uint8_t prog_write_text[] = {
    0x48, 0xb8, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc6, 0x00, 0x00,
};
/*   mov rax, USER_STACK ; jmp rax   (NX page: #PF) */
static const uint8_t prog_exec_stack[] = {
    0x48, 0xb8, 0x00, 0xf0, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xe0,
};

static const struct {
    const char *name;
    const uint8_t *code;
    size_t len;
} bad_progs[] = {
    {"hlt", prog_hlt, sizeof prog_hlt},
    {"cli", prog_cli, sizeof prog_cli},
    {"int3", prog_int3, sizeof prog_int3},
    {"read_kernel", prog_read_kernel, sizeof prog_read_kernel},
    {"write_text", prog_write_text, sizeof prog_write_text},
    {"exec_stack", prog_exec_stack, sizeof prog_exec_stack},
};

/* Each faults in ring 3: it dies on its own, the kernel lives, and nothing
 * leaks. */
int test_user_fault_kills(void) {
    size_t before = free_frames();
    for (size_t i = 0; i < sizeof bad_progs / sizeof bad_progs[0]; i++) {
        KASSERT(run_user(bad_progs[i].name, bad_progs[i].code,
                         bad_progs[i].len) != NULL);
        KASSERT(wait_threads_done());
        KASSERT(free_frames() == before);
    }
    return 0;
}

/* A user mapping in the kernel half would be shared by every process. */
int test_user_map_kernel_half(void) {
    uint64_t pml4 = vmm_space_new();
    uint64_t frame = pmm_alloc();
    KASSERT(pml4 != 0 && frame != 0);
    KASSERT(vmm_space_map(pml4, TEST_VIRT, frame, 0) == -EINVAL);
    KASSERT(vmm_space_map(pml4, USER_TOP, frame, 0) == -EINVAL);
    KASSERT(vmm_space_map(pml4, USER_TOP - PAGE_SIZE, frame, 0) == 0);
    vmm_space_free(pml4); /* frees `frame` too */
    return 0;
}

#endif /* ifdef WARDEN_TEST */
