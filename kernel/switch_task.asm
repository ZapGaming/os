; void switch_task(uint32_t *old_esp_store, uint32_t new_esp, void *old_fpu, void *new_fpu)
; Saves the callee-saved registers of the currently running task onto its
; own stack, records that stack pointer, then switches to the new task's
; stack and restores its callee-saved registers. The final `ret` resumes
; execution wherever the new task last left off (or its trampoline, on
; first run) since that address is sitting on top of its stack.
;
; Also saves/restores the x87 FPU register file across the switch, so a
; task using floats can't corrupt or be corrupted by another task's FPU
; state. eax/ecx/edx are all caller-saved under cdecl (nothing needs to
; preserve them across this call), which is exactly enough scratch
; registers to juggle four incoming pointer/value arguments without
; touching ebx/esi/edi/ebp before they're pushed -- old_fpu is consumed
; immediately (fnsave), new_fpu is kept in edx across the stack switch
; (nothing between here and the frstor touches edx).
global switch_task
switch_task:
    mov eax, [esp+12]   ; old_fpu
    fnsave [eax]

    mov eax, [esp+4]    ; old_esp_store
    mov ecx, [esp+8]    ; new_esp
    mov edx, [esp+16]   ; new_fpu

    push ebx
    push esi
    push edi
    push ebp

    mov [eax], esp
    mov esp, ecx

    pop ebp
    pop edi
    pop esi
    pop ebx

    frstor [edx]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
