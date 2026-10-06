; gdt.asm - load the GDT and reload segment registers
bits 64
default rel

section .text

; void gdt_load(const struct gdt_ptr *ptr, uint16_t code, uint16_t data)
;   rdi = ptr, si = code selector, dx = data selector
global gdt_load
gdt_load:
    lgdt [rdi]

    ; Data segments can be loaded with a plain mov.
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax        ; FS/GS bases are set via MSRs later, not selectors
    mov fs, ax
    mov gs, ax

    ; CS can't be mov'd: fake a far call frame and "return" into the new CS.
    movzx rsi, si
    push rsi            ; new CS
    lea rax, [.reloaded]
    push rax            ; new RIP
    retfq
.reloaded:
    ret
