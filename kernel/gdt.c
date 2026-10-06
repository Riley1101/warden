#include "gdt.h"

#include <stddef.h>

/* 
* 64-bit TSS: no tasks, just the stacks the CPU switches to. 
* @see https://wiki.osdev.org/Task_State_Segment
*/
struct tss {
  uint32_t reserved0;
  uint64_t rsp[3]; /* rsp[0]: stack for ring 3 -> ring 0 transitions */
  uint64_t reserved1;
  uint64_t ist[7]; /* ist[n-1]: stack for IDT gates with IST = n */
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb; /* I/O bitmap offset; >= limit means "no bitmap" */
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "TSS must be 104 bytes");

struct gdt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));


/**
* Access byte bits , 
* @see https://wiki.osdev.org/Global_Descriptor_Table#Access_Byte
*/
#define A_PRESENT (1 << 7)
#define A_DPL3    (3 << 5)
#define A_SEG     (1 << 4) /* code/data (vs system, e.g. TSS) */
#define A_EXEC    (1 << 3)
#define A_RW      (1 << 1) /* readable code / writable data */
#define A_TSS64   0x9      /* system type: available 64-bit TSS */

/* Flags nibble */
#define F_GRAN4K (1 << 3)
#define F_SIZE32 (1 << 2) /* data segments; must be 0 when F_LONG is set */
#define F_LONG   (1 << 1) /* 64-bit code segment */

/* Base and limit are ignored for code/data in long mode, but we fill in the
 * conventional flat values so a debugger shows something sane. */
static uint64_t seg(uint8_t access, uint8_t flags) {
  uint64_t limit = 0xFFFFF;
  return (limit & 0xFFFF) | ((uint64_t)access << 40) |
         ((limit >> 16) & 0xF) << 48 | ((uint64_t)flags << 52);
}

enum {
  GDT_NULL_IDX,
  GDT_KCODE_IDX,
  GDT_KDATA_IDX,
  GDT_UDATA_IDX,
  GDT_UCODE_IDX,
  GDT_TSS_IDX, /* a TSS descriptor takes two slots */
  GDT_ENTRIES = GDT_TSS_IDX + 2,
};

/* Not const: ltr writes the "busy" bit into the TSS descriptor. */
static uint64_t gdt[GDT_ENTRIES];
static struct tss tss;

#define IST_STACK_SIZE 16384
static uint8_t df_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

/* gdt.asm: lgdt + reload CS via far return + reload data segments */
extern void gdt_load(const struct gdt_ptr *ptr, uint16_t code, uint16_t data);

/**
 * Initialize the Global Descriptor Table (GDT).
 * @see https://wiki.osdev.org/GDT_Tutorial#Flat_/_Long_Mode_Setup
 */
void gdt_init(void) {
  gdt[GDT_NULL_IDX] = 0;
  gdt[GDT_KCODE_IDX] =
      seg(A_PRESENT | A_SEG | A_EXEC | A_RW, F_GRAN4K | F_LONG);
  gdt[GDT_KDATA_IDX] = seg(A_PRESENT | A_SEG | A_RW, F_GRAN4K | F_SIZE32);
  gdt[GDT_UDATA_IDX] =
      seg(A_PRESENT | A_DPL3 | A_SEG | A_RW, F_GRAN4K | F_SIZE32);
  gdt[GDT_UCODE_IDX] =
      seg(A_PRESENT | A_DPL3 | A_SEG | A_EXEC | A_RW, F_GRAN4K | F_LONG);

  /* A dedicated stack for #DF: if the kernel stack overflows, the fault
   * handler can't use it, and without IST we'd triple fault silently. */
  tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(df_stack + sizeof df_stack);
  tss.iopb = sizeof tss;

  /* 16-byte system descriptor: base is split across both slots. */
  uint64_t base = (uint64_t)&tss;
  uint64_t limit = sizeof tss - 1;
  gdt[GDT_TSS_IDX] = (limit & 0xFFFF) | (base & 0xFFFFFF) << 16 |
                     (uint64_t)(A_PRESENT | A_TSS64) << 40 |
                     ((limit >> 16) & 0xF) << 48 |
                     ((base >> 24) & 0xFF) << 56;
  gdt[GDT_TSS_IDX + 1] = base >> 32;

  struct gdt_ptr ptr = {sizeof gdt - 1, (uint64_t)gdt};
  gdt_load(&ptr, GDT_KCODE, GDT_KDATA);
  __asm__ __volatile__("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_rsp0(uint64_t rsp0) { tss.rsp[0] = rsp0; }
