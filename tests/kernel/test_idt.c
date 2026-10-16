#ifdef WARDEN_TEST
#include "ktest.h"
#include "idt.h"

static int bp_hits;
static uint64_t bp_rip;

static void on_breakpoint(struct trap_frame *f) {
    bp_hits++;
    bp_rip = f->rip;
}

/* int3 is a trap: the handler runs and execution resumes after it. */
int test_idt_breakpoint(void) {
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

int test_idt_frame(void) {
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

#endif /* ifdef WARDEN_TEST */
