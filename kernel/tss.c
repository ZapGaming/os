#include <kernel/tss.h>
#include <kernel/gdt.h>
#include <kernel/serial.h>
#include <string.h>

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0, ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* One TSS per logical CPU (see tss.h's TSS_MAX_CPUS) -- the prior pass
 * had exactly one, global, on the theory that only one core ever ran
 * kernel code at a time. That's no longer true once the AP takes
 * interrupts of its own (kernel/apic.c's ap_main()): esp0 is the ring-0
 * stack the CPU switches to on a ring3->ring0 transition, and if two
 * cores shared one TSS, one core's transition could clobber the esp0
 * the other core's transition just set up. Only esp0/ss0 are
 * meaningfully used in either entry, same as before -- this kernel does
 * software task-switching (kernel/switch_task.asm), not hardware
 * task-switching, so no other TSS field is ever read by the CPU here. */
static struct tss_entry tss[TSS_MAX_CPUS];

extern void tss_flush(uint16_t selector);

void tss_init(void) {
    memset(tss, 0, sizeof(tss));
    for (int i = 0; i < TSS_MAX_CPUS; i++) {
        tss[i].ss0 = 0x10;   /* kernel data selector, same for every CPU */
        tss[i].esp0 = 0;     /* set per-task via tss_set_kernel_stack_cpu() before entering ring3 */
        tss[i].iomap_base = sizeof(tss[i]); /* no I/O bitmap: ring3 code can't use in/out */
    }

    gdt_set_gate(5, (uint32_t)&tss[0], sizeof(tss[0]) - 1, 0x89, 0x00); /* BSP  -- selector 0x28 */
    gdt_set_gate(6, (uint32_t)&tss[1], sizeof(tss[1]) - 1, 0x89, 0x00); /* AP   -- selector 0x30 */

    /* Only the BSP loads TR here -- the AP doesn't exist yet at this
     * point in boot (kernel_main() calls this long before kernel/smp.c
     * decides whether to wake one at all), and LTR is a per-CPU
     * register load the AP must do for itself once it does exist (see
     * tss_load_ap(), called from kernel/apic.c's ap_main()). */
    tss_flush(TSS_SELECTOR);

    serial_printf("tss: BSP TSS installed at %x (selector %x), AP TSS reserved at %x (selector %x, not yet loaded)\n",
                  (uint32_t)&tss[0], TSS_SELECTOR, (uint32_t)&tss[1], TSS_SELECTOR_AP);
}

void tss_load_ap(void) {
    tss_flush(TSS_SELECTOR_AP);
    serial_printf("tss: AP loaded its own TSS (selector %x)\n", TSS_SELECTOR_AP);
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss[0].esp0 = esp0;
}

void tss_set_kernel_stack_cpu(int cpu_index, uint32_t esp0) {
    if (cpu_index < 0 || cpu_index >= TSS_MAX_CPUS) return;
    tss[cpu_index].esp0 = esp0;
}
