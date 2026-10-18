#pragma once

#include <idt.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Programs: ELF files in the filesystem, loaded into fresh address spaces.
 *
 * A new process's stack (16 pages, top at USTACK_TOP) starts, at rsp:
 *   argc, argv[0] .. argv[argc - 1], NULL, NULL (empty envp)
 * with the strings above that, in the top page. rsp is 16-byte aligned.
 */

#define USTACK_TOP   0x00007ffffffff000UL
#define USTACK_PAGES 16
#define PROC_MAX_ARGS 32
#define HEAP_MAX     (256UL << 20) /* how far brk may move past its start */

/* Start `path` with `argv` as a new process, child of `parent` (or NULL).
 * Returns its pid; fs_open's errors, -EACCES (a directory), -ENOEXEC,
 * -E2BIG, -ENOMEM. */
struct thread;
int proc_spawn(const char *path, const char *const *argv, int argc,
               struct thread *parent);

/*
 * Replace the calling process's program with `path`: on success its old
 * address space is gone and `f` (its trap frame) returns into the new
 * program. On failure nothing has changed. Errors as proc_spawn.
 */
int proc_exec(struct trap_frame *f, const char *path, const char *const *argv,
              int argc);

/*
 * Move the caller's heap end to `addr`, mapping fresh zeroed pages or
 * freeing them. Returns the new end, or the unchanged one if `addr` is
 * below the heap's start, more than HEAP_MAX past it, or memory ran out
 * (so brk(0) asks where it is).
 */
uint64_t proc_brk(uint64_t addr);
