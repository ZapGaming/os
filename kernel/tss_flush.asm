; void tss_flush(uint16_t selector)
; LTR is a per-CPU register load (like LGDT/LIDT) -- each core that wants
; its own esp0 on a ring3->ring0 transition must load its OWN TSS
; selector into TR itself, hence the selector is now a parameter rather
; than the hardcoded BSP one (GDT index 5) it used to always be. See
; kernel/tss.c's tss_init() (BSP, selector 0x28) and tss_load_ap()
; (AP, selector 0x30).
global tss_flush
tss_flush:
    mov eax, [esp+4]
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
