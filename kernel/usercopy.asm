; usercopy.asm - the only code that touches user memory
;
; Each instruction that may fault on a user address gets an .extable entry
; {instruction, fixup}. The #PF handler resumes a kernel fault on a user
; address at the fixup (usercopy.c: extable_fixup).
bits 64
default rel

section .text

; int usercopy(void *dst, const void *src, size_t n)
; 0, or 1 if a user page faulted. The caller checks the range is user.
global usercopy
usercopy:
    stac                ; SMAP: allow user access until clac
    mov rcx, rdx
.copy:
    rep movsb           ; may fault
    clac
    xor eax, eax
    ret
.fault:
    clac
    mov eax, 1
    ret

section .extable progbits alloc noexec nowrite align=8
    dq usercopy.copy, usercopy.fault
