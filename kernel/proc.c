#include "proc.h"

#include <abi/errno.h>
#include <cpu.h>
#include <elf.h>
#include <file.h>
#include <kstring.h>
#include <pmm.h>
#include <sched.h>
#include <slab.h>
#include <tar.h>
#include <vmm.h>

static const void *initrd;
static size_t initrd_len;

static uint64_t page_up(uint64_t x) {
  return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

void proc_init(const void *base, size_t len) {
  initrd = base;
  initrd_len = len;
}

/* Map a fresh zeroed frame at `virt`. */
static int map_zeroed(uint64_t pml4, uint64_t virt, uint64_t flags) {
  uint64_t frame = pmm_alloc();
  if (frame == 0)
    return -ENOMEM;
  memset(phys_to_virt(frame), 0, PAGE_SIZE);
  int err = vmm_space_map(pml4, virt, frame, flags);
  if (err != 0)
    pmm_free(frame);
  return err;
}

static int load_segments(uint64_t pml4, const uint8_t *img,
                         const struct elf_image *elf) {
  for (int i = 0; i < elf->nseg; i++) {
    const struct elf_segment *s = &elf->seg[i];
    uint64_t flags = (s->writable ? PTE_W : 0) | (s->exec ? 0 : PTE_NX);
    uint64_t start = s->vaddr & ~(PAGE_SIZE - 1);
    for (uint64_t v = start; v < s->vaddr + s->memsz; v += PAGE_SIZE) {
      int err = map_zeroed(pml4, v, flags);
      if (err != 0)
        return err;
    }
    int err = vmm_space_write(pml4, s->vaddr, img + s->offset, s->filesz);
    if (err != 0)
      return err;
  }
  return 0;
}

/* Map the stack and lay out argc/argv in its top page. Sets *rsp. */
static int load_stack(uint64_t pml4, const char *const *argv, int argc,
                      uint64_t *rsp) {
  if (argc < 0 || argc > PROC_MAX_ARGS)
    return -E2BIG;
  for (int i = 1; i <= USTACK_PAGES; i++) {
    int err = map_zeroed(pml4, USTACK_TOP - i * PAGE_SIZE, PTE_W | PTE_NX);
    if (err != 0)
      return err;
  }

  uint8_t *page = kzalloc(PAGE_SIZE);
  if (page == NULL)
    return -ENOMEM;
  const uint64_t base = USTACK_TOP - PAGE_SIZE; /* user address of `page` */
  uint64_t uargv[PROC_MAX_ARGS];
  size_t top = PAGE_SIZE;
  int err = 0;
  for (int i = argc - 1; i >= 0 && err == 0; i--) {
    size_t n = strlen(argv[i]) + 1;
    if (n > top) {
      err = -E2BIG;
      break;
    }
    top -= n;
    memcpy(page + top, argv[i], n);
    uargv[i] = base + top;
  }
  /* argc, argv[], NULL, envp NULL; argc lands 16-byte aligned */
  size_t words = (size_t)argc + 3;
  if (err == 0 && top < words * 8 + 16)
    err = -E2BIG;
  if (err == 0) {
    size_t sp = (top - words * 8) & ~(size_t)15;
    uint64_t *w = (uint64_t *)(page + sp);
    w[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
      w[1 + i] = uargv[i];
    w[1 + argc] = 0;
    w[2 + argc] = 0;
    err = vmm_space_write(pml4, base, page, PAGE_SIZE);
    *rsp = base + sp;
  }
  kfree(page);
  return err;
}

/* `path` with `argv`, in a new address space. Sets *pml4 and the entry. */
/* A program loaded into a new address space, not yet running. */
struct loaded {
  uint64_t pml4, rip, rsp;
  uint64_t brk; /* the heap starts empty at the first page after the image */
};

static int load(const char *path, const char *const *argv, int argc,
                struct loaded *out) {
  const void *img;
  size_t len;
  if (tar_find(initrd, initrd_len, path, &img, &len) != 0)
    return -ENOENT;
  struct elf_image elf;
  if (elf_parse(img, len, &elf) != 0)
    return -ENOEXEC;

  uint64_t pml4 = vmm_space_new();
  if (pml4 == 0)
    return -ENOMEM;
  int err = load_segments(pml4, img, &elf);
  if (err == 0)
    err = load_stack(pml4, argv, argc, &out->rsp);
  if (err != 0) {
    vmm_space_free(pml4);
    return err;
  }
  out->pml4 = pml4;
  out->rip = elf.entry;
  out->brk = 0;
  for (int i = 0; i < elf.nseg; i++) {
    uint64_t end = page_up(elf.seg[i].vaddr + elf.seg[i].memsz);
    if (end > out->brk)
      out->brk = end;
  }
  return 0;
}

/* The last component: what a process is called. */
static const char *basename(const char *path) {
  const char *b = path;
  for (const char *p = path; *p != '\0'; p++)
    if (*p == '/' && p[1] != '\0')
      b = p + 1;
  return b;
}

int proc_spawn(const char *path, const char *const *argv, int argc,
               struct thread *parent) {
  struct loaded l;
  int err = load(path, argv, argc, &l);
  if (err != 0)
    return err;
  struct trap_frame tf;
  trap_frame_user(&tf, l.rip, l.rsp);
  /* Give it its files and heap, and read its pid, before it can run, die
   * and be freed. */
  uint64_t flags = irq_save();
  struct thread *t = thread_create_user(basename(path), l.pml4, &tf, parent);
  int pid = -ENOMEM;
  if (t != NULL) {
    fd_init_console(t->fd);
    t->brk_base = t->brk = l.brk;
    pid = t->tid;
  }
  irq_restore(flags);
  return pid;
}

int proc_exec(struct trap_frame *f, const char *path, const char *const *argv,
              int argc) {
  struct loaded l;
  int err = load(path, argv, argc, &l);
  if (err != 0)
    return err;
  struct thread *t = thread_current();
  uint64_t old = t->pml4;
  t->pml4 = l.pml4;
  vmm_switch(l.pml4);
  vmm_space_free(old); /* out of CR3 now; the old heap goes with it */
  t->brk_base = t->brk = l.brk;
  thread_set_name(t, basename(path));
  trap_frame_user(f, l.rip, l.rsp);
  return 0;
}

uint64_t proc_brk(uint64_t addr) {
  struct thread *t = thread_current();
  if (addr < t->brk_base || addr - t->brk_base > HEAP_MAX)
    return t->brk; /* includes brk(0): just asking */

  uint64_t old_top = page_up(t->brk), new_top = page_up(addr);
  for (uint64_t v = old_top; v < new_top; v += PAGE_SIZE) {
    if (map_zeroed(t->pml4, v, PTE_W | PTE_NX) != 0) {
      for (uint64_t u = old_top; u < v; u += PAGE_SIZE) /* undo */
        pmm_free(vmm_space_unmap(t->pml4, u));
      return t->brk;
    }
  }
  for (uint64_t v = new_top; v < old_top; v += PAGE_SIZE)
    pmm_free(vmm_space_unmap(t->pml4, v));
  t->brk = addr;
  return addr;
}
