; Real entry point. GRUB jumps here in 32-bit protected mode with:
;   eax = 0x36d76289 (multiboot2 magic)
;   ebx = physical address of the multiboot2 info structure
section .bss
align 16
stack_bottom:
    resb 65536                 ; 64 KiB kernel stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    cli
    mov esp, stack_top
    mov ebp, esp

    push ebx                    ; multiboot info pointer -> 2nd arg
    push eax                    ; multiboot magic        -> 1st arg
    call kernel_main

    ; kernel_main should never return, but halt forever if it does
.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
