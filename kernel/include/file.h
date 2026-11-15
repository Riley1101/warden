#pragma once

#include <stddef.h>
#include <stdint.h>

struct node; /* fs.h */

/*
 * Open files and file descriptor tables. A file is shared (refcounted) by
 * every descriptor that names it, across fork and dup2. Kinds: the console
 * (write goes to COM1, read takes a typed line), pipe ends, and fs nodes.
 *
 * Callers keep interrupts off (syscalls run with them off): a pipe
 * operation may sleep, and tables and refcounts have no other lock.
 */

#define NFD 16 /* descriptors per process */

/* The vector COM1's interrupt line is delivered on. */
#define T_CONSOLE 0x24

/*
 * Start taking console input on COM1's interrupt. Call after apic_init().
 */
void console_init(void);

/*
 * Take whatever has been typed on COM1: echo it, gather it into a line,
 * and hand finished lines to anyone reading the console. Called from the
 * console interrupt, and from the timer tick as a backstop, with
 * interrupts off.
 */
void console_poll(void);

/* One byte as if it had been typed. Interrupts must be off. */
void console_feed(int c);

struct file;

/* Descriptors 0, 1, 2 on the console: what a new program starts with. */
void fd_init_console(struct file **table);

/* `table` gets a reference to every file `src` has open (fork). */
void fd_copy(struct file **table, struct file *const *src);

/* The file open at `fd`, or NULL. */
struct file *fd_get(struct file **table, int fd);

/* 0, or -EBADF. */
int fd_close(struct file **table, int fd);

/* Close every descriptor (a process dying). */
void fd_close_all(struct file **table);

/* Make `newfd` name what `oldfd` does, closing whatever `newfd` had.
 * Returns newfd; -EBADF if `oldfd` isn't open or `newfd` is out of range. */
int fd_dup2(struct file **table, int oldfd, int newfd);

/* Open `path` (fs_open's flags and errors) at the lowest free descriptor,
 * and return it; -EMFILE if none is free. */
int fd_open(struct file **table, const char *path, int flags);

/* Mark the node just created at the caller's request as the caller's:
 * its writer label (fs.h), so stat and exec see who made it. */
void file_note_creator(struct node *n);

/* The next entry of the directory open at `fd`: 1 and *out filled, 0 after
 * the last; -EBADF, -ENOTDIR. */
struct dirent;
int fd_getdent(struct file **table, int fd, struct dirent *out);

/* A pipe: fds[0] reads what fds[1] writes. 0, -EMFILE, or -ENOMEM. */
int fd_pipe(struct file **table, int fds[2]);

/*
 * Read or write through a user buffer. Bytes moved, 0 at end of file, or
 * -EBADF (wrong direction), -EFAULT, -EPIPE (writing with no reader left),
 * -EISDIR, -EFBIG. Files read and write at their offset (O_APPEND: writes
 * at the end) and move it.
 * Pipes block: a read until there is data or no writer, a write until it
 * has all gone in or the last reader is gone.
 */
int64_t file_read(struct file *f, void *ubuf, size_t n);
int64_t file_write(struct file *f, const void *ubuf, size_t n);
