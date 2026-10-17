; crt0.asm - where every user program starts
;
; The kernel leaves rsp 16-byte aligned at argc, then argv[] (kernel/proc.h).
; main's return value is the exit code.
bits 64
default rel

SYS_exit equ 0          ; abi/syscall.h

extern main

section .text
global _start
_start:
    xor ebp, ebp        ; outermost frame
    mov rdi, [rsp]      ; argc
    lea rsi, [rsp + 8]  ; argv
    call main
    mov edi, eax
    mov eax, SYS_exit
    int 0x80
    ud2
