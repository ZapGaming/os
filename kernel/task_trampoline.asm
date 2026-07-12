; Entry point for a brand-new task's very first run. switch_task's `ret`
; lands here (never via a real interrupt), so interrupts are still off from
; the last `cli` and must be explicitly re-enabled before we run real code.
global task_trampoline
extern task_exited
task_trampoline:
    sti
    pop eax          ; real entry point, pushed just below this address
    call eax
    call task_exited
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
