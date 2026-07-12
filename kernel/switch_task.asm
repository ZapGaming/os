; void switch_task(uint32_t *old_esp_store, uint32_t new_esp)
; Saves the callee-saved registers of the currently running task onto its
; own stack, records that stack pointer, then switches to the new task's
; stack and restores its callee-saved registers. The final `ret` resumes
; execution wherever the new task last left off (or its trampoline, on
; first run) since that address is sitting on top of its stack.
global switch_task
switch_task:
    mov eax, [esp+4]    ; old_esp_store
    mov edx, [esp+8]    ; new_esp

    push ebx
    push esi
    push edi
    push ebp

    mov [eax], esp
    mov esp, edx

    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
