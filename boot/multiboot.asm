; Multiboot2 header - tells GRUB how to load and set up this kernel.
; Requests a linear graphics framebuffer so the kernel never has to touch
; real-mode VBE/VGA BIOS calls itself.
section .multiboot_header
align 8
header_start:
    dd 0xe85250d6                ; magic number
    dd 0                         ; architecture: 0 = i386 protected mode
    dd header_end - header_start ; header length
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

    ; framebuffer request tag: ask for a 1920x1080x32 linear framebuffer.
    ; This is a request, not a guarantee -- GRUB's video driver picks the
    ; closest mode it actually has and reports back whatever it granted
    ; in the multiboot info framebuffer tag, which is what the kernel
    ; actually uses (see kernel/multiboot.c); every part of the GUI reads
    ; fb_width()/fb_height() at runtime rather than assuming a fixed size.
    align 8
    dw 5                         ; type = framebuffer
    dw 0                         ; flags
    dd 20                        ; size
    dd 1920                      ; width
    dd 1080                      ; height
    dd 32                        ; depth

    ; end tag
    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .note.GNU-stack noalloc noexec nowrite progbits
