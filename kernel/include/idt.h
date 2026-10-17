#pragma once

#include <stdint.h>

/*
 * Register state saved on every interrupt/exception, in stack order
 * (lowest address first). idt.asm builds this; keep the two in sync.
 */
struct trap_frame {
  /* pushed by idt_common */
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
  /* pushed by the per-vector stub (error is 0 if the CPU pushed none) */
  uint64_t vector, error;
  /* pushed by the CPU */
  uint64_t rip, cs, rflags, rsp, ss;
};

_Static_assert(sizeof(struct trap_frame) == 22 * 8, "trap_frame layout");

/* Exception vectors used by name elsewhere */
#define T_DIVIDE       0
#define T_NMI          2
#define T_BREAKPOINT   3
#define T_INVALID_OPCODE 6
#define T_DOUBLE_FAULT 8
#define T_GP           13
#define T_PAGE_FAULT   14
#define T_MACHINE_CHECK 18

/*
 * A handler may modify the frame; changes are written back to the
 * registers on return (this is how syscalls will return values).
 */
typedef void (*idt_handler_t)(struct trap_frame *f);

/* Build and load the IDT. Call after gdt_init(). */
void idt_init(void);

/* Install `fn` for `vector`, replacing the default (dump + panic). */
void idt_register(uint8_t vector, idt_handler_t fn);
