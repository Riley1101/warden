#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Parse a static x86_64 ELF executable into the segments to load. Pure:
 * reads `img` only, maps nothing, so it is host-tested on malformed input.
 * Must stay free of hardware code.
 */

#define ELF_MAX_SEGMENTS 8

struct elf_segment {
  uint64_t vaddr, memsz;   /* where, and how much (the tail past filesz is 0) */
  uint64_t offset, filesz; /* the bytes in the image */
  int writable, exec;      /* never both */
};

struct elf_image {
  uint64_t entry;
  int nseg;
  struct elf_segment seg[ELF_MAX_SEGMENTS];
};

/*
 * Returns 0, or -ENOEXEC unless all of this holds: ELF64, little endian,
 * x86_64, ET_EXEC, no interpreter; 1..ELF_MAX_SEGMENTS non-empty PT_LOAD
 * segments (empty ones are skipped),
 * each inside the image and inside [PAGE_SIZE, USER_TOP), not writable and
 * executable at once, none sharing a page with another; and the entry
 * point inside an executable segment.
 */
int elf_parse(const void *img, size_t len, struct elf_image *out);
