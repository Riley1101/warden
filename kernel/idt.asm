; idt.asm - interrupt entry stubs
;
; Every vector gets a tiny stub that makes the stack look the same
; (push a dummy error code if the CPU didn't, then the vector number) and
; jumps to idt_common, which saves the GPRs into a struct trap_frame and
; calls idt_dispatch(frame).
bits 64
default rel

extern idt_dispatch

section .text

; Vectors where the CPU pushes an error code itself:
; #DF(8) #TS(10) #NP(11) #SS(12) #GP(13) #PF(14) #AC(17) #CP(21) #VC(29) #SX(30)
%macro STUB 1
idt_stub_%1:
%if %1 = 8 || (%1 >= 10 && %1 <= 14) || %1 = 17 || %1 = 21 || %1 = 29 || %1 = 30
%else
    push 0              ; dummy error code
%endif
    push %1             ; vector
    jmp idt_common
%endmacro

%assign i 0
%rep 256
    STUB i
%assign i i + 1
%endrep

; TODO(user mode): swapgs on entry/exit when coming from ring 3.
idt_common:
    ; Ring 3 can set RFLAGS.AC and the CPU keeps it on entry, which would
    ; switch SMAP off for the whole handler. iretq restores the old value.
    clac
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; The CPU aligns RSP to 16 before pushing its 5-word frame, and
    ; 5 + 2 + 15 = 22 words is even, so RSP is 16-byte aligned here as
    ; the SysV ABI requires at a call.
    cld
    mov rdi, rsp
    call idt_dispatch

; A new user thread starts here (sched.c): its stack holds just a
; trap_frame, which this pops into the registers and irets to.
global trap_return
trap_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16         ; drop vector + error code
    iretq

; Table of stub addresses, indexed by vector, for idt_init().
section .rodata
global idt_stubs
idt_stubs:
%assign i 0
%rep 256
    dq idt_stub_ %+ i
%assign i i + 1
%endrep
