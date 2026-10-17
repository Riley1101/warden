#include "check.h"
#include "elf.h"

#include <abi/errno.h>
#include <stdint.h>

/* A minimal valid image: text at 0x400000 (R-X), data at 0x401000 (RW-). */
struct image {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
  struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
  } ph[3];
  uint8_t body[64];
};

static struct image good(void) {
  struct image im = {
      .ident = {0x7f, 'E', 'L', 'F', 2, 1, 1},
      .type = 2,
      .machine = 62,
      .version = 1,
      .entry = 0x400010,
      .phoff = 64,
      .ehsize = 64,
      .phentsize = 56,
      .phnum = 2,
  };
  uint64_t body = offsetof(struct image, body);
  im.ph[0].type = 1;
  im.ph[0].flags = 5; /* R X */
  im.ph[0].offset = body;
  im.ph[0].vaddr = 0x400000;
  im.ph[0].filesz = 32;
  im.ph[0].memsz = 32;
  im.ph[1].type = 1;
  im.ph[1].flags = 6; /* R W */
  im.ph[1].offset = body + 32;
  im.ph[1].vaddr = 0x401000;
  im.ph[1].filesz = 16;
  im.ph[1].memsz = 0x2000; /* .bss tail */
  return im;
}

static int parse(const struct image *im) {
  struct elf_image out;
  return elf_parse(im, sizeof *im, &out);
}

static void test_good(void) {
  struct image im = good();
  struct elf_image out;
  CHECK(elf_parse(&im, sizeof im, &out) == 0);
  CHECK(out.entry == 0x400010);
  CHECK(out.nseg == 2);
  CHECK(out.seg[0].exec && !out.seg[0].writable);
  CHECK(out.seg[1].writable && !out.seg[1].exec);
  CHECK(out.seg[1].memsz == 0x2000 && out.seg[1].filesz == 16);

  /* other program headers, and empty PT_LOADs, are skipped */
  im.phnum = 3;
  im.ph[2].type = 4; /* PT_NOTE */
  CHECK(parse(&im) == 0);
  im.ph[2].type = 1;
  im.ph[2].vaddr = 0x400000; /* would overlap text if it counted */
  CHECK(elf_parse(&im, sizeof im, &out) == 0 && out.nseg == 2);
}

#define BAD(field_change)                                                      \
  do {                                                                         \
    struct image im = good();                                                  \
    field_change;                                                              \
    CHECK(parse(&im) == -ENOEXEC);                                             \
  } while (0)

static void test_header(void) {
  struct image im = good();
  struct elf_image out;
  CHECK(elf_parse(&im, 63, &out) == -ENOEXEC); /* truncated header */
  CHECK(elf_parse(NULL, 0, &out) == -ENOEXEC);
  /* program headers run past the end */
  CHECK(elf_parse(&im, 64 + 56, &out) == -ENOEXEC);

  BAD(im.ident[0] = 0);
  BAD(im.ident[4] = 1);  /* 32-bit */
  BAD(im.ident[5] = 2);  /* big endian */
  BAD(im.type = 3);      /* ET_DYN */
  BAD(im.machine = 3);   /* i386 */
  BAD(im.version = 0);
  BAD(im.phentsize = 32);
  BAD(im.phnum = 0);
  BAD(im.phnum = 65);
  BAD(im.phoff = UINT64_MAX - 8); /* wraps */
}

static void test_segments(void) {
  BAD(im.ph[0].filesz = 64);                      /* filesz > memsz */
  BAD(im.ph[0].memsz = 0; im.ph[0].filesz = 0); /* text skipped: no entry */
  BAD(im.ph[0].offset = sizeof im);               /* bytes past the end */
  BAD(im.ph[0].offset = UINT64_MAX; im.ph[0].filesz = 2); /* wraps */
  BAD(im.ph[0].vaddr = 0; im.entry = 0x10);       /* null page */
  BAD(im.ph[1].vaddr = 0x00007ffffffff000UL);     /* .bss runs past USER_TOP */
  BAD(im.ph[1].vaddr = 0xffffffff80000000UL);     /* kernel half */
  BAD(im.ph[1].memsz = UINT64_MAX);               /* wraps */
  BAD(im.ph[0].flags = 7);                        /* W+X */
  BAD(im.ph[1].vaddr = 0x400800);                 /* shares text's page */
  BAD(im.ph[0].type = 3);                         /* PT_INTERP */
  BAD(im.ph[0].type = 4; im.ph[1].type = 4);      /* nothing to load */
  BAD(im.entry = 0x401000);                       /* entry in data */
  BAD(im.entry = 0x400020);                       /* entry past text */
}

int main(void) {
  test_good();
  test_header();
  test_segments();
  return CHECK_DONE();
}
