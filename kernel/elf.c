#include "elf.h"

#include <abi/errno.h>
#include <kstring.h>
#include <vmm.h>

/* The parts of the ELF64 format we read. */
struct ehdr {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
  uint32_t type, flags;
  uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

_Static_assert(sizeof(struct ehdr) == 64, "ELF64 header");
_Static_assert(sizeof(struct phdr) == 56, "ELF64 program header");

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_INTERP   3
#define PF_X        1
#define PF_W        2

#define MAX_PHNUM 64

static uint64_t page_down(uint64_t x) { return x & ~(PAGE_SIZE - 1); }
static uint64_t page_up(uint64_t x) { return page_down(x + PAGE_SIZE - 1); }

/* [off, off + n) inside [0, len), without wrapping. */
static int inside(uint64_t off, uint64_t n, uint64_t len) {
  return off <= len && n <= len - off;
}

static int load_segment(const struct phdr *p, size_t len,
                        struct elf_segment *s) {
  if (p->filesz > p->memsz)
    return -ENOEXEC;
  if (!inside(p->offset, p->filesz, len))
    return -ENOEXEC;
  /* page-rounded end must stay in the user half */
  if (p->vaddr < PAGE_SIZE || p->vaddr >= USER_TOP ||
      p->memsz > USER_TOP - p->vaddr || page_up(p->vaddr + p->memsz) > USER_TOP)
    return -ENOEXEC;
  if ((p->flags & PF_W) && (p->flags & PF_X))
    return -ENOEXEC; /* W^X */
  *s = (struct elf_segment){
      .vaddr = p->vaddr,
      .memsz = p->memsz,
      .offset = p->offset,
      .filesz = p->filesz,
      .writable = !!(p->flags & PF_W),
      .exec = !!(p->flags & PF_X),
  };
  return 0;
}

/* Segments are mapped whole pages at a time: two sharing a page would need
 * one frame with both permissions. */
static int pages_overlap(const struct elf_segment *a,
                         const struct elf_segment *b) {
  return page_down(a->vaddr) < page_up(b->vaddr + b->memsz) &&
         page_down(b->vaddr) < page_up(a->vaddr + a->memsz);
}

int elf_parse(const void *img, size_t len, struct elf_image *out) {
  struct ehdr h;
  if (img == NULL || len < sizeof h)
    return -ENOEXEC;
  memcpy(&h, img, sizeof h); /* the image need not be aligned */

  if (memcmp(h.ident, "\x7f" "ELF", 4) != 0 || h.ident[4] != ELFCLASS64 ||
      h.ident[5] != ELFDATA2LSB || h.ident[6] != 1 || h.type != ET_EXEC ||
      h.machine != EM_X86_64 || h.version != 1 ||
      h.phentsize != sizeof(struct phdr) || h.phnum == 0 ||
      h.phnum > MAX_PHNUM ||
      !inside(h.phoff, (uint64_t)h.phnum * sizeof(struct phdr), len))
    return -ENOEXEC;

  out->entry = h.entry;
  out->nseg = 0;
  for (int i = 0; i < h.phnum; i++) {
    struct phdr p;
    memcpy(&p, (const uint8_t *)img + h.phoff + i * sizeof p, sizeof p);
    if (p.type == PT_INTERP)
      return -ENOEXEC; /* dynamically linked */
    if (p.type != PT_LOAD || p.memsz == 0)
      continue; /* an empty segment (no .rodata, say) loads nothing */
    if (out->nseg == ELF_MAX_SEGMENTS)
      return -ENOEXEC;
    struct elf_segment *s = &out->seg[out->nseg];
    if (load_segment(&p, len, s) != 0)
      return -ENOEXEC;
    for (int j = 0; j < out->nseg; j++)
      if (pages_overlap(s, &out->seg[j]))
        return -ENOEXEC;
    out->nseg++;
  }
  if (out->nseg == 0)
    return -ENOEXEC;

  for (int i = 0; i < out->nseg; i++) {
    const struct elf_segment *s = &out->seg[i];
    if (s->exec && h.entry >= s->vaddr && h.entry - s->vaddr < s->memsz)
      return 0;
  }
  return -ENOEXEC; /* entry point not in executable code */
}
