#pragma once

/* Format and print to the COM1 console. Output longer than 256 bytes is
 * truncated. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Print "PANIC: <msg>" and stop the machine. Under WARDEN_TEST this exits
 * QEMU with a failure code so the test run fails instead of hanging. */
__attribute__((noreturn)) void panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
