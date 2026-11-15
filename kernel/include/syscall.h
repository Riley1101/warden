#pragma once

#include <stdint.h>

/* Route the syscall vector to dispatch(). Call after idt_init(). */
void syscall_init(void);

/* An audit record (abi/audit.h) about the calling process, in the
 * syscall it is making: for refusals decided further in (a file's
 * writer, an exec's label). `path` may be NULL. */
void syscall_audit(uint32_t kind, uint32_t need, int err, const char *path);
