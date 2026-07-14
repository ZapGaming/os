#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

void gdt_init(void);
void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);

/* Reloads THIS CPU's GDTR (and CS/DS/etc segment registers) to point at
 * the one shared GDT gdt_init() already built -- LGDT is a per-CPU
 * register load even though the table it points to is ordinary shared
 * memory, so every core that wants to use this GDT (including its own
 * TSS descriptor -- see kernel/tss.c) must execute this itself. Must be
 * called after gdt_init() has already run (once, on the BSP); safe to
 * call from any CPU, any number of times. */
void gdt_load_this_cpu(void);

#endif
