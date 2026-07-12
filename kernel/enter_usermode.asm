; void enter_usermode(void (*entry)(void), uint32_t user_stack_top)
;
; Drops the CPU from ring 0 to ring 3 via iret, into `entry` running on
; `user_stack_top`. This never returns -- ring3 code can only get back to
; ring0 via an interrupt (a syscall, or a fault).
global enter_usermode
enter_usermode:
    cli
    mov eax, [esp+4]    ; entry point
    mov ecx, [esp+8]    ; user stack top

    mov dx, 0x23         ; user data selector (GDT index 4, RPL 3)
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    push dword 0x23      ; SS (user data selector)
    push ecx             ; ESP
    pushf
    pop ebx
    or ebx, 0x200         ; guarantee IF is set once we're in ring3
    push ebx              ; EFLAGS
    push dword 0x1B       ; CS (user code selector, GDT index 3, RPL 3)
    push eax               ; EIP
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
