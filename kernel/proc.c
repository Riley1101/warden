#include "proc.h"

#include <abi/audit.h>
#include <abi/errno.h>
#include <cpu.h>
#include <elf.h>
#include <file.h>
#include <fs.h>
#include <kstring.h>
#include <pmm.h>
#include <sched.h>
#include <session.h>
#include <slab.h>
#include <syscall.h>
#include <vmm.h>

static uint64_t page_up(uint64_t x) {
  return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
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

/* A program loaded into a new address space, not yet running. */
struct loaded {
  uint64_t pml4, rip, rsp;
  uint64_t brk; /* the heap starts empty at the first page after the image */
  struct label writer; /* who wrote what runs: the script and its interpreter */
  uint32_t writer_max; /* that session's process limit */
};

/* Fold the writer label of `file` into `out`'s: what runs carries both. */
static int take_writer(struct loaded *out, struct node *file) {
  uint32_t max;
  const struct label *w = fs_writer(file, &max);
  int err = label_meet(&out->writer, w);
  if (err == 0 && w->origin == ORIGIN_AGENT &&
      (out->writer_max == 0 || max < out->writer_max))
    out->writer_max = max;
  return err;
}

/* The interpreter a "#!" script names, into `out` (PATH_MAX bytes): 1, 0
 * if `img` isn't a script, or -ENOEXEC if the line is bad. */
static int shebang(const uint8_t *img, uint64_t size, char *out) {
  if (size < 2 || img[0] != '#' || img[1] != '!')
    return 0;
  uint64_t i = 2, n = 0;
  while (i < size && img[i] == ' ')
    i++;
  while (i < size && img[i] != '\n' && img[i] != ' ') {
    if (n == PATH_MAX - 1)
      return -ENOEXEC;
    out[n++] = (char)img[i++];
  }
  out[n] = '\0';
  return n > 0 ? 1 : -ENOEXEC;
}

static int load_elf(const char *path, const char *const *argv, int argc,
                    struct loaded *out, int depth);

/*
 * `path` with `argv`, in a new address space. A "#!" script runs its
 * interpreter instead, as `interp path argv[1...]`, one level deep. The
 * writer label of every file involved ends up in out->writer.
 */
static int load(const char *path, const char *const *argv, int argc,
                struct loaded *out) {
  out->pml4 = 0; /* until it has an address space */
  out->writer = (struct label){0};
  out->writer_max = 0;
  return load_elf(path, argv, argc, out, 0);
}

static int load_elf(const char *path, const char *const *argv, int argc,
                    struct loaded *out, int depth) {
  struct node *file;
  int err = fs_open(path, O_RDONLY, &file);
  if (err != 0)
    return err;
  struct elf_image elf;
  const uint8_t *img = fs_data(file);
  uint64_t pml4 = 0;
  char interp[PATH_MAX];
  int script = fs_is_dir(file) ? 0 : shebang(img, fs_size(file), interp);
  if (script != 0) {
    err = script < 0 || depth > 0 ? -ENOEXEC : take_writer(out, file);
    fs_put(file); /* it runs as the interpreter's input, not as code */
    if (err != 0)
      return err;
    if (argc + 1 > PROC_MAX_ARGS)
      return -E2BIG;
    const char *args[PROC_MAX_ARGS];
    args[0] = interp;
    args[1] = path;
    for (int i = 1; i < argc; i++)
      args[i + 1] = argv[i];
    return load_elf(interp, args, argc < 1 ? 2 : argc + 1, out, depth + 1);
  }
  /* -ECAPDENIED from take_writer: two sessions wrote what would run */
  err = fs_is_dir(file) ? -EACCES : take_writer(out, file);
  if (err == 0 && elf_parse(img, fs_size(file), &elf) != 0)
    err = -ENOEXEC;
  if (err == 0 && (pml4 = vmm_space_new()) == 0)
    err = -ENOMEM;
  if (err == 0)
    err = load_segments(pml4, img, &elf);
  if (err == 0)
    err = load_stack(pml4, argv, argc, &out->rsp);
  fs_put(file); /* its bytes are in the new address space now */
  if (err != 0) {
    if (pml4 != 0)
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

/*
 * The label a process with label `cur` takes on to run `l`: narrowed by
 * whoever wrote it, so an agent's file runs as that agent, whoever runs
 * it. A human joining the writer's session counts there from now (and
 * makes the session again if it has ended). Into *cur; 0, -ECAPDENIED
 * (another session's file), or -EAGAIN (the session is full).
 */
static int adopt(struct label *cur, const struct loaded *l) {
  struct label next = *cur;
  int err = label_meet(&next, &l->writer);
  if (err == 0 && cur->origin == ORIGIN_HUMAN && next.origin == ORIGIN_AGENT)
    err = session_enter(next.session, l->writer_max);
  if (err == 0)
    *cur = next;
  return err;
}

int proc_spawn(const char *path, const char *const *argv, int argc,
               struct thread *parent) {
  /* Interrupts off throughout, as in a syscall: the file can't change
   * while it loads, and the child gets its files and heap, and we its pid,
   * before it can run, die and be freed. */
  uint64_t flags = irq_save();
  struct loaded l;
  struct label label = {0}; /* human, unless the file says otherwise */
  int err = load(path, argv, argc, &l);
  if (err == 0)
    err = adopt(&label, &l);
  if (err != 0) {
    if (l.pml4 != 0)
      vmm_space_free(l.pml4);
    irq_restore(flags);
    return err;
  }
  struct trap_frame tf;
  trap_frame_user(&tf, l.rip, l.rsp);
  struct thread *t = thread_create_user(basename(path), l.pml4, &tf, parent);
  int pid = -ENOMEM;
  if (t != NULL) {
    fd_init_console(t->fd);
    t->brk_base = t->brk = l.brk;
    t->label = label;
    pid = t->tid;
  } else if (label.origin == ORIGIN_AGENT) {
    session_leave(label.session); /* it never ran */
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
  err = adopt(&t->label, &l);
  if (err != 0) {
    vmm_space_free(l.pml4);
    syscall_audit(AUDIT_DENIED, CAP_SPAWN, err, path);
    return err;
  }
  if (l.writer.origin == ORIGIN_AGENT) /* runs as the file's writer */
    syscall_audit(AUDIT_MARK, t->label.caps, 0, path);
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
