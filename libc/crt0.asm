; crt0.asm - where every user program starts
;
; The kernel leaves rsp 16-byte aligned at argc, then argv[] (kernel/proc.h).
; main's return value goes to exit().
bits 64
default rel

extern main
extern exit

section .text
global _start
_start:
    xor ebp, ebp        ; outermost frame
    mov rdi, [rsp]      ; argc
    lea rsi, [rsp + 8]  ; argv
    call main
    mov edi, eax
    call exit
    ud2
