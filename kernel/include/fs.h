#pragma once

#include <abi/fs.h>
#include <cap.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The in-memory filesystem (abi/fs.h has the user's view). Pure: nodes and
 * contents live in kmalloc'd memory, so it is host-tested over the real
 * slab allocator. Must stay free of hardware code.
 *
 * Every path is absolute and goes through path_normalize here, so ".",
 * ".." and "//" can't reach the tree. A node is freed once it is unlinked
 * and no open file refers to it (fs_put).
 *
 * No locking: callers keep interrupts off (one CPU), as for files.
 */

#define FS_FILE_MAX (16UL << 20) /* bytes in one file */

struct node;

/* An empty tree: just "/". Call once, before anything else. */
int fs_init(void);

/* Add every file and directory in the ustar archive `tar`, creating parent
 * directories as needed. 0, or the first error (the tree keeps what came
 * before it). */
int fs_load_tar(const void *tar, size_t len);

/*
 * Open `path` and take a reference to it, which fs_put drops. `flags` as
 * open(2): O_CREAT makes a missing file (its directory must exist),
 * O_TRUNC empties it. A directory opens only read-only (else -EISDIR).
 * -ENOENT, -ENOTDIR (a component isn't a directory), -ENAMETOOLONG.
 */
int fs_open(const char *path, int flags, struct node **out);
void fs_put(struct node *n);

int fs_mkdir(const char *path);   /* -EEXIST if anything is there */
int fs_unlink(const char *path);  /* a file, or an empty directory */
int fs_rename(const char *from, const char *to);
int fs_stat(const char *path, struct stat *st);
void fs_fstat(struct node *n, struct stat *st);

int fs_is_dir(struct node *n);

/*
 * Read or write at `off` from/into kernel memory. Read returns the bytes
 * read (0 at or past the end); write returns n, extending the file (any
 * gap reads as zeros), or -EFBIG past FS_FILE_MAX, -ENOMEM.
 */
int64_t fs_read(struct node *n, uint64_t off, void *dst, size_t len);
int64_t fs_write(struct node *n, uint64_t off, const void *src, size_t len);
uint64_t fs_size(struct node *n);

/* The `index`th entry of directory `n`: 1 and *out filled, or 0 past the
 * last one. */
int fs_readdir(struct node *n, uint64_t index, struct dirent *out);

/*
 * The writer label: who wrote a file, for exec to hand on (cap.h). Zeroed
 * (human) for a new file and everything from the initramfs. A write by
 * an agent narrows it by the agent's label (label_meet), with its
 * session's process limit; a human's write leaves it be. Sticky: only
 * unlinking the file clears it, and rename moves it along.
 *
 * fs_note_writer: before a write by a process labelled `l`, its session
 * allowing `max_procs`. 0, or -ECAPDENIED if another session's agent
 * wrote it: a file takes one session's label, never two.
 */
int fs_note_writer(struct node *n, const struct label *l, uint32_t max_procs);
/* The writer label of `n`, and its session limit into *max_procs. */
const struct label *fs_writer(struct node *n, uint32_t *max_procs);

/* A file's whole contents, for exec (valid until it is written or put). */
const void *fs_data(struct node *n);
