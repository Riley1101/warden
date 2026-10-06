#pragma once

#include <stdint.h>

/** 
 * Segment selectors.
 * 
 * The GDT is laid out as follows:
 * - 0x08: kernel code
 * - 0x10: kernel data
 * - 0x18: user data
 * - 0x20: user code
 * - 0x28: task state segment
 */
#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA (0x18 | 3)
#define GDT_UCODE (0x20 | 3)
#define GDT_TSS   0x28

#define IST_DOUBLE_FAULT 1

/* Load the GDT, reload every segment register, and load the TSS. */
void gdt_init(void);

/* Stack the CPU switches to when an interrupt arrives in ring 3. */
void tss_set_rsp0(uint64_t rsp0);
