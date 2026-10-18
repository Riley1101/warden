#pragma once

/* Route the syscall vector to dispatch(). Call after idt_init(). */
void syscall_init(void);
