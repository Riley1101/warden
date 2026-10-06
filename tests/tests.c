#ifdef WARDEN_TEST
#include "fmt.h"
#include "ktest.h"
#include "path.h"
#include "pmm.h"
#include "idt.h"

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
};
const int ktests_count = sizeof(ktests) / sizeof(ktests[0]);

#endif /* ifdef WARDEN_TEST */
