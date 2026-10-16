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

GDT_UDATA   equ 0x18 | 3    ; gdt.h
GDT_UCODE   equ 0x20 | 3
RFLAGS_USER equ 0x202       ; IF, plus bit 1 which is always set

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

; noreturn void enter_user(uint64_t rip, uint64_t rsp)
; Drop to ring 3 at rip with stack rsp, interrupts on. Clears every GPR so
; no kernel value leaks to user space.
global enter_user
enter_user:
    cli                 ; no interrupt between building the frame and iretq
    push GDT_UDATA      ; ss
    push rsi            ; rsp
    push RFLAGS_USER    ; rflags
    push GDT_UCODE      ; cs
    push rdi            ; rip
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

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
