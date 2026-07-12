global tss_flush
tss_flush:
    mov ax, 0x28   ; TSS selector, GDT index 5
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
