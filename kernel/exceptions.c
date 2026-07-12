#include <kernel/exceptions.h>
#include <kernel/idt.h>
#include <kernel/serial.h>

static const char *exception_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound range", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection fault", "Page fault", "Reserved",
    "x87 FP exception", "Alignment check", "Machine check", "SIMD FP exception",
    "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security exception", "Reserved",
};

static void exception_handler(struct registers *regs) {
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    serial_printf("\n*** CPU EXCEPTION %d (%s) ***\n", regs->int_no, exception_names[regs->int_no]);
    serial_printf("err=%x eip=%x cs=%x eflags=%x cr2=%x\n", regs->err_code, regs->eip, regs->cs, regs->eflags, cr2);
    serial_printf("eax=%x ebx=%x ecx=%x edx=%x esi=%x edi=%x ebp=%x\n",
                  regs->eax, regs->ebx, regs->ecx, regs->edx, regs->esi, regs->edi, regs->ebp);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void exceptions_init(void) {
    for (int i = 0; i < 32; i++) {
        register_interrupt_handler(i, exception_handler);
    }
}
