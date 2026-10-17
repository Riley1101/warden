#ifdef WARDEN_TEST
#include "ktest.h"
#include "kstring.h"
#include "pmm.h"
#include "sched.h"
#include "slab.h"
#include "timer.h"
#include "vmm.h"

#include <abi/errno.h>
#include <abi/syscall.h>
#include <abi/wait.h>

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
    struct trap_frame tf;
    trap_frame_user(&tf, USER_CODE, USER_STACK_TOP);
    return thread_create_user(name, pml4, &tf, NULL);
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

    thread_kill(t, W_SIGNALED(SIGKILL));
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
/* Floating point is off in ring 3 (its state is not saved per thread).
 * Each spins after the instruction, so surviving it hangs the test. */
/*   fninit ; jmp $   (CR0.EM: #NM) */
static const uint8_t prog_x87[] = {0xdb, 0xe3, 0xeb, 0xfe};
/*   xorps xmm0, xmm0 ; jmp $   (CR0.EM: #UD) */
static const uint8_t prog_sse[] = {0x0f, 0x57, 0xc0, 0xeb, 0xfe};

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
    {"x87", prog_x87, sizeof prog_x87},
    {"sse", prog_sse, sizeof prog_sse},
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

/* ---- syscalls ----------------------------------------------------------- */

#define SYSCALL       0xcd, 0x80                        /* int 0x80 */
#define MOV_EAX(n)    0xb8, (n), 0x00, 0x00, 0x00       /* mov eax, n (< 256) */
#define SAVE_RAX                                        /* mov [USER_STACK], rax */ \
    0xbb, 0x00, 0xf0, 0x7f, 0x00, /* mov ebx, USER_STACK */                    \
    0x48, 0x89, 0x03              /* mov [rbx], rax */
#define EXIT          MOV_EAX(SYS_exit), SYSCALL
#define HANG          0xeb, 0xfe                        /* jmp $ */

/* What the program left at USER_STACK. Read before the thread is reaped:
 * its frames are still ours until then. */
static int64_t saved_rax(void) {
    return *(volatile int64_t *)phys_to_virt(user_stack_frame);
}

/* exit returns to nothing: the thread is gone, and so is all it had. */
static const uint8_t prog_exit[] = {EXIT, HANG};

int test_syscall_exit(void) {
    size_t before = free_frames();
    KASSERT(run_user("exit", prog_exit, sizeof prog_exit) != NULL);
    KASSERT(wait_threads_done());
    KASSERT(free_frames() == before);
    return 0;
}

/* A number past the table, and one that is negative as a signed value. */
static const uint8_t prog_enosys[] = {
    MOV_EAX(200), SYSCALL, SAVE_RAX, EXIT, HANG,
};
static const uint8_t prog_enosys_neg[] = {
    0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, /* mov rax, -1 */
    SYSCALL, SAVE_RAX, EXIT, HANG,
};

int test_syscall_enosys(void) {
    KASSERT(run_user("enosys", prog_enosys, sizeof prog_enosys) != NULL);
    KASSERT(wait_threads_done());
    KASSERT(saved_rax() == -ENOSYS);

    KASSERT(run_user("enosys_neg", prog_enosys_neg, sizeof prog_enosys_neg) !=
            NULL);
    KASSERT(wait_threads_done());
    KASSERT(saved_rax() == -ENOSYS);
    return 0;
}

/*   debug_write(buf, len) with both as 64-bit immediates; patched per case */
#define DW_BUF_AT 7  /* offset of buf's imm64 */
#define DW_LEN_AT 17 /* offset of len's imm64 */
static const uint8_t prog_debug_write[] = {
    MOV_EAX(SYS_debug_write),
    0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, /* mov rdi, buf */
    0x48, 0xbe, 0, 0, 0, 0, 0, 0, 0, 0, /* mov rsi, len */
    SYSCALL, SAVE_RAX, EXIT, HANG,
    'o', 'k', '\n', /* the message, at the end of the code page's code */
};
#define DW_MSG_AT (sizeof prog_debug_write - 3)

/* debug_write(buf, len) from ring 3; what it returned. */
static int64_t debug_write_returns(uint64_t buf, uint64_t len) {
    uint8_t code[sizeof prog_debug_write];
    memcpy(code, prog_debug_write, sizeof code);
    memcpy(code + DW_BUF_AT, &buf, 8);
    memcpy(code + DW_LEN_AT, &len, 8);
    if (run_user("debug_write", code, sizeof code) == NULL ||
        !wait_threads_done())
        return 1; /* never a valid result here */
    return saved_rax();
}

/* Prints "ok\n" from a user buffer. */
int test_syscall_debug_write(void) {
    KASSERT(debug_write_returns(USER_CODE + DW_MSG_AT, 3) == 3);
    KASSERT(debug_write_returns(USER_CODE, 0) == 0);
    return 0;
}

/* Bad user pointers get -EFAULT; the process carries on (and exits), the
 * kernel never dereferences them for real. */
int test_syscall_efault(void) {
    size_t before = free_frames();
    KASSERT(debug_write_returns(0, 1) == -EFAULT);           /* unmapped */
    KASSERT(debug_write_returns(0xffffffff80000000UL, 1) == -EFAULT); /* kernel */
    KASSERT(debug_write_returns(USER_TOP - 1, 2) == -EFAULT); /* past top */
    /* starts mapped, faults partway (the page after the code is unmapped) */
    KASSERT(debug_write_returns(USER_CODE + PAGE_SIZE - 2, 4) == -EFAULT);
    KASSERT(free_frames() == before);
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
