; Raw flat 16-bit -> 32-bit real-mode trampoline for waking one
; Application Processor (AP) via INIT-SIPI-SIPI.
;
; This file is NOT linked into the normal kernel ELF image the way every
; other .asm file in this tree is (see the Makefile's dedicated
; `nasm -f bin` rule for this one file, and the exclusion of this file
; from the generic ASM_SOURCES glob) -- a normal `-f elf32` object gets
; linked wherever the linker feels like putting it (>1MB here), but this
; code MUST execute starting at a fixed, known, sub-1MB physical address,
; because that is where the CPU lands in REAL MODE immediately after
; SIPI (CS:IP = vector*0x100 : 0x0000, i.e. physical vector*0x1000) --
; real mode can't reach an address up in the kernel's normal >1MB load
; region at all.
;
; At runtime, kernel/apic.c's apic_start_ap() memcpy()s the assembled
; flat binary verbatim to the fixed physical address
; AP_TRAMPOLINE_PHYS_ADDR (0x8000 -- see include/kernel/apic.h, which
; MUST match the ORG below), then sends SIPI with a vector encoding that
; same address (vector = addr >> 12), before patching the two runtime
; values at the very end of this file (see ap_stack_top_ptr/ap_entry_ptr
; below) and finally triggering the wake sequence.
;
; Because ORG here is hardcoded to the exact physical address this blob
; is always copied to, every absolute address referenced below (the GDT
; pointer, the far jump target) is already correct as assembled -- no
; runtime relocation of this file's own internal addresses is needed,
; only the two AP-specific values (stack top, C entry point) that can't
; be known until the kernel is actually running.

BITS 16
ORG 0x8000

ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load OUR OWN tiny, self-contained flat GDT (defined below) --
    ; deliberately separate from the kernel's real GDT (kernel/gdt.c).
    ; The AP only needs this to flip into 32-bit protected mode; the C
    ; entry point below (ap_main, via kernel/apic.c) keeps running under
    ; this same temporary GDT forever, since a parked AP that never
    ; enters ring 3 and never takes an interrupt needs nothing the
    ; kernel's own GDT/TSS would add (see include/kernel/smp.h's scope
    ; note on per-CPU GDT/TSS).
    lgdt [gdt_ptr]

    mov eax, cr0
    or eax, 1               ; CR0.PE
    mov cr0, eax

    ; Far jump into the 32-bit code segment (selector 0x08 in our tiny
    ; GDT, not the kernel's). `jmp dword` forces a 32-bit offset to be
    ; encoded even though we're still nominally in a 16-bit BITS
    ; section at this point -- the standard incantation for this exact
    ; real-mode-to-protected-mode transition.
    jmp dword 0x08:pm32_entry

BITS 32
pm32_entry:
    mov ax, 0x10            ; our tiny GDT's flat data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Both of these are patched by kernel/apic.c's apic_start_ap()
    ; *before* SIPI is sent -- see ap_stack_top_ptr/ap_entry_ptr below.
    mov esp, [ap_stack_top_ptr]
    mov eax, [ap_entry_ptr]
    call eax                ; -> ap_main() in kernel/apic.c; never returns

.hang:                      ; just in case it ever did
    hlt
    jmp .hang

; --- Our own tiny flat GDT: null, flat 32-bit code (0x08), flat 32-bit
; data (0x10). Base 0, limit 4GB, exactly like the access/granularity
; bytes kernel/gdt.c uses for its own kernel-mode descriptors.
align 8
gdt_start:
    dq 0                    ; null descriptor

    dw 0xFFFF               ; code: limit 0-15
    dw 0x0000                ; code: base 0-15
    db 0x00                  ; code: base 16-23
    db 0x9A                  ; code: access (present, ring0, code, exec/read)
    db 0xCF                  ; code: flags(4)+limit 16-19(4) = granularity 4K, 32-bit
    db 0x00                  ; code: base 24-31

    dw 0xFFFF                ; data: limit 0-15
    dw 0x0000                ; data: base 0-15
    db 0x00                  ; data: base 16-23
    db 0x92                  ; data: access (present, ring0, data, read/write)
    db 0xCF                  ; data: flags+limit, same as code
    db 0x00                  ; data: base 24-31
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; --- Runtime-patched values -- MUST stay the last two dwords in this
; file: kernel/apic.c computes their address as
; (blob_end - 8) and (blob_end - 4) rather than via a symbol table
; (this blob has none once assembled with `-f bin`). Do not add
; anything after these two without updating AP_STACK_PATCH_OFFSET_FROM_END
; / AP_ENTRY_PATCH_OFFSET_FROM_END in kernel/apic.c.
align 4
ap_stack_top_ptr: dd 0      ; patched: top of this AP's kmalloc'd stack
ap_entry_ptr:      dd 0      ; patched: address of ap_main() in kernel/apic.c

ap_trampoline_end:
