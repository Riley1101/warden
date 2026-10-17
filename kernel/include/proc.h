#pragma once

#include <idt.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Programs: ELF files in the initramfs, loaded into fresh address spaces.
 *
 * A new process's stack (16 pages, top at USTACK_TOP) starts, at rsp:
 *   argc, argv[0] .. argv[argc - 1], NULL, NULL (empty envp)
 * with the strings above that, in the top page. rsp is 16-byte aligned.
 */

#define USTACK_TOP   0x00007ffffffff000UL
#define USTACK_PAGES 16
#define PROC_MAX_ARGS 32

/* The initramfs (a ustar archive), from the bootloader. */
void proc_init(const void *initrd, size_t len);

/* Start `path` with `argv` as a new process, child of `parent` (or NULL).
 * Returns its pid, or -ENOENT, -ENOEXEC, -E2BIG, -ENOMEM. */
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
