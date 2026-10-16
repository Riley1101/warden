; sched.asm - kernel thread context switch
;
; A thread that isn't running is parked inside switch_to: its callee-saved
; registers on its own stack, and that stack pointer in thread->rsp.
; Caller-saved registers need no saving: switch_to is an ordinary call.
; A preempted thread is the same, just deeper: below switch_to on its stack
; sit the timer handler and the trap_frame idt_common saved.
bits 64
default rel

extern thread_exit

section .text

; void switch_to(uint64_t *old_rsp, uint64_t new_rsp)
global switch_to
switch_to:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First code a new thread runs: thread_create's fake switch_to frame
; returns here with fn in rbx and arg in r12. Interrupts are off (we came
; from schedule()), so turn them on before running the thread.
global thread_trampoline
thread_trampoline:
    sti
    mov rdi, r12
    call rbx
    call thread_exit
    ud2
