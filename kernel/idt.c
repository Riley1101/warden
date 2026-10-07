#include "idt.h"

#include <cpu.h>
#include <gdt.h>
#include <kprintf.h>
#include <stddef.h>



/**
 * Initialize the Interrupt Descriptor Table (IDT).
 * @see https://wiki.osdev.org/Interrupt_Descriptor_Table
 */
struct idt_gate {
  uint16_t off_lo; // offset bits 0..15
  uint16_t selector; // code segment selector in GDT or LDT
  uint8_t ist; // bits 0-2: IST slot, 0 = don't switch stacks *
  uint8_t type_attr; // gate type, dpl, p
  uint16_t off_mid; // offset bits 16..31
  uint32_t off_hi; // offset bits 32..63
  uint32_t reserved; // reserved, set to 0
} __attribute__((packed));

_Static_assert(sizeof(struct idt_gate) == 16, "IDT gate must be 16 bytes");

struct idt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

/* Present, DPL 0, 64-bit interrupt gate (clears IF on entry). */
#define GATE_INT 0x8E

static struct idt_gate idt[256];
static idt_handler_t handlers[256];

extern const uint64_t idt_stubs[256]; /* idt.asm */

/**
 * Set an IDT gate to point to a trap handler.
 * @param vector The interrupt vector number (0-255).
 * @param addr The address of the trap handler function.
 * @param ist The Interrupt Stack Table (IST) index to use for this gate (0 means no stack switch).
 */
static void set_gate(uint8_t vector, uint64_t addr, uint8_t ist) {
  idt[vector] = (struct idt_gate){
      .off_lo = addr & 0xFFFF,
      .selector = GDT_KCODE,
      .ist = ist,
      .type_attr = GATE_INT,
      .off_mid = (addr >> 16) & 0xFFFF,
      .off_hi = addr >> 32,
  };
}

/**
 *  Initialize the Interrupt Descriptor Table (IDT) and load it into the CPU.
 */
void idt_init(void) {
  for (int v = 0; v < 256; v++)
    set_gate(v, idt_stubs[v], 0);
  set_gate(T_DOUBLE_FAULT, idt_stubs[T_DOUBLE_FAULT], IST_DOUBLE_FAULT);

  struct idt_ptr ptr = {sizeof idt - 1, (uint64_t)idt};
  __asm__ __volatile__("lidt %0" : : "m"(ptr));
}

/**
 * Register a trap handler for a specific interrupt vector.
 * @param vector The interrupt vector number (0-255).
 * @param fn The function to call when the interrupt occurs.
 */
void idt_register(uint8_t vector, idt_handler_t fn) { handlers[vector] = fn; }

static const char *const exception_names[32] = {
    "#DE divide error",    "#DB debug",
    "NMI",                 "#BP breakpoint",
    "#OF overflow",        "#BR bound range",
    "#UD invalid opcode",  "#NM device not available",
    "#DF double fault",    "coprocessor segment overrun",
    "#TS invalid TSS",     "#NP segment not present",
    "#SS stack fault",     "#GP general protection",
    "#PF page fault",      "reserved",
    "#MF x87 FP error",    "#AC alignment check",
    "#MC machine check",   "#XM SIMD FP error",
    "#VE virtualization",  "#CP control protection",
    "reserved",            "reserved",
    "reserved",            "reserved",
    "reserved",            "reserved",
    "#HV hypervisor",      "#VC VMM communication",
    "#SX security",        "reserved",
};

/**
 * Dump the contents of a trap frame to the console.
 * @param f The trap frame to dump.
 */
static void dump_frame(const struct trap_frame *f) {
  kprintf("  rip=%016lx cs=%04lx rflags=%016lx\n", f->rip, f->cs, f->rflags);
  kprintf("  rsp=%016lx ss=%04lx err=%016lx\n", f->rsp, f->ss, f->error);
  kprintf("  rax=%016lx rbx=%016lx rcx=%016lx\n", f->rax, f->rbx, f->rcx);
  kprintf("  rdx=%016lx rsi=%016lx rdi=%016lx\n", f->rdx, f->rsi, f->rdi);
  kprintf("  rbp=%016lx r8 =%016lx r9 =%016lx\n", f->rbp, f->r8, f->r9);
  kprintf("  r10=%016lx r11=%016lx r12=%016lx\n", f->r10, f->r11, f->r12);
  kprintf("  r13=%016lx r14=%016lx r15=%016lx\n", f->r13, f->r14, f->r15);
}

/**
 * Dispatch a trap to the appropriate handler.
 * @param f The trap frame containing the CPU state at the time of the trap.
 */
void idt_dispatch(struct trap_frame *f) {
  idt_handler_t fn = handlers[f->vector];
  if (fn != NULL) {
    fn(f);
    return;
  }

  // If we get here, it means the interrupt is not handled by any registered handler.
  // This is a serious error, so we panic.
  if (f->vector < 32) {
    kprintf("\n%s (vector %lu)\n", exception_names[f->vector], f->vector);
    if (f->vector == T_PAGE_FAULT) {
      /* error bits: P(0) W(1) U(2) RSVD(3) I(4) */
      kprintf("  cr2=%016lx (%s, %s, %s mode%s)\n", read_cr2(),
              f->error & 1 ? "protection" : "not present",
              f->error & 2 ? "write" : "read", f->error & 4 ? "user" : "kernel",
              f->error & 16 ? ", instruction fetch" : "");
    }
    dump_frame(f);
    panic("unhandled exception %lu at rip %016lx", f->vector, f->rip);
  }

  dump_frame(f);
  panic("unexpected interrupt %lu", f->vector);
}
