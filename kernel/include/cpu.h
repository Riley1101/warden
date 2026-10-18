#pragma once

#include <stdint.h>

/* Control registers, MSRs and TLB helpers. Kernel only. */

#define MSR_EFER  0xC0000080
#define EFER_NXE  (1UL << 11) /* allow the NX bit in page tables */
#define CR0_EM    (1UL << 2)  /* no x87/SSE: they trap (#NM / #UD) */
#define CR0_WP    (1UL << 16) /* ring 0 honours read-only pages too */

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
  __asm__ __volatile__("wrmsr" : : "c"(msr), "a"((uint32_t)v),
                       "d"((uint32_t)(v >> 32)));
}

static inline uint64_t read_cr0(void) {
  uint64_t v;
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(v));
  return v;
}

static inline void write_cr0(uint64_t v) {
  __asm__ __volatile__("mov %0, %%cr0" : : "r"(v) : "memory");
}

#define CR4_SMEP  (1UL << 20) /* ring 0 can't execute user pages */
#define CR4_SMAP  (1UL << 21) /* ring 0 can't touch user pages, except
                                 between stac and clac (usercopy) */

/* CPUID leaf 7, EBX */
#define CPUID7_SMEP (1u << 7)
#define CPUID7_SMAP (1u << 20)

static inline uint64_t read_cr4(void) {
  uint64_t v;
  __asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
  return v;
}

static inline void write_cr4(uint64_t v) {
  __asm__ __volatile__("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline uint64_t read_cr2(void) {
  uint64_t v;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
  return v;
}

static inline uint64_t read_cr3(void) {
  uint64_t v;
  __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
  return v;
}

/* Switch address space; flushes all non-global TLB entries. */
static inline void write_cr3(uint64_t v) {
  __asm__ __volatile__("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void invlpg(uint64_t virt) {
  __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c,
                         uint32_t *d) {
  __asm__ __volatile__("cpuid"
                       : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                       : "a"(leaf), "c"(0));
}

#define RFLAGS_IF (1UL << 9)

/* Disable interrupts, returning the old RFLAGS for irq_restore(). On one
 * CPU this is the lock around anything an interrupt handler also touches. */
static inline uint64_t irq_save(void) {
  uint64_t flags;
  __asm__ __volatile__("pushfq\n\tpop %0\n\tcli" : "=r"(flags) : : "memory");
  return flags;
}

static inline void irq_restore(uint64_t flags) {
  if (flags & RFLAGS_IF)
    __asm__ __volatile__("sti" : : : "memory");
}
