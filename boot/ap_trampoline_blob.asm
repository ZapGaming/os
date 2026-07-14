; Thin wrapper that embeds the flat AP trampoline binary (assembled
; separately -- see boot/ap_trampoline.asm and the Makefile's dedicated
; `nasm -f bin` rule for it) as a byte array inside the normal kernel
; ELF image, so kernel/apic.c can reach it as ordinary linked read-only
; data and memcpy() it down to low memory at runtime. This file itself
; IS a normal part of the kernel build (picked up by the Makefile's
; ASM_SOURCES glob and assembled -f elf32 like everything else) -- only
; boot/ap_trampoline.asm needs the special flat-binary treatment.
section .rodata
global ap_trampoline_blob
global ap_trampoline_blob_end

ap_trampoline_blob:
    incbin "build/boot/ap_trampoline.bin"
ap_trampoline_blob_end:

section .note.GNU-stack noalloc noexec nowrite progbits
