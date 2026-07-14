#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include <stdint.h>

struct registers {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};

typedef void (*isr_handler_t)(struct registers *);

void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);

/* Reloads THIS CPU's IDTR to point at the one shared IDT idt_init()
 * already built -- LIDT is a per-CPU register load even though the
 * table it points to (and the handlers[] dispatch table behind
 * isr_dispatch()) is ordinary shared memory. Must be called after
 * idt_init() has already run (once, on the BSP); safe to call from any
 * CPU, any number of times. See kernel/apic.c's ap_main(), the only
 * other caller. */
void idt_load_this_cpu(void);

#endif
