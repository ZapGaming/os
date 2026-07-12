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

static struct tss_entry tss;

extern void tss_flush(void);

/* Only esp0/ss0 (the ring-0 stack the CPU switches to on a ring3 -> ring0
 * transition, e.g. an interrupt firing while in user mode) matter here --
 * we don't use hardware task-switching, just this one field of the TSS. */
void tss_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = 0x10;   /* kernel data selector */
    tss.esp0 = 0;     /* set per-task via tss_set_kernel_stack before entering ring3 */
    tss.iomap_base = sizeof(tss); /* no I/O bitmap: ring3 code can't use in/out */

    gdt_set_gate(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);
    tss_flush();

    serial_printf("tss: installed at %x\n", (uint32_t)&tss);
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}
