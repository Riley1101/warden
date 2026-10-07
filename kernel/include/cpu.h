#pragma once

#include <stdint.h>

/* Control registers, MSRs and TLB helpers. Kernel only. */

#define MSR_EFER  0xC0000080
#define EFER_NXE  (1UL << 11) /* allow the NX bit in page tables */
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
