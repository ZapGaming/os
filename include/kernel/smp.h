#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

/* Best-effort, safe-by-default entry point for this pass's SMP bring-up
 * MVP: detects CPUs via ACPI/MADT (kernel/acpi.c), and if -- and only
 * if -- more than one is reported, wakes exactly ONE additional core
 * (an "AP") via the Local APIC's INIT-SIPI-SIPI sequence
 * (kernel/apic.c) and proves it is genuinely executing independently of
 * the boot CPU (a heartbeat counter the AP increments in a busy loop,
 * read back here after a short delay). On a single-CPU system, or if
 * ACPI/MADT parsing fails for any reason, this is a clean no-op with no
 * behavior change from before this pass existed -- it must NEVER hang
 * or crash a boot that would otherwise have worked.
 *
 * Explicit scope cuts (see the task-level writeup for the full
 * reasoning) -- this function, and everything it calls, deliberately
 * does NOT:
 *   - touch kernel/scheduler.c's schedule()/tasks[]/current_task, or
 *     run any task/scheduling code on the AP at all;
 *   - route PIC or IOAPIC interrupts through the APIC (the 8259 stays
 *     the sole interrupt source for both CPUs);
 *   - give the AP its own GDT/TSS (it runs forever under the
 *     trampoline's own tiny temporary flat GDT -- fine, since it never
 *     enters ring 3 or takes an interrupt needing esp0);
 *   - wake more than one AP;
 *   - add any locking primitive (spinlock/atomic) anywhere -- see the
 *     thread-safety note on kernel/apic.c's ap_main().
 * Call this once from kernel_main(), after interrupts are enabled
 * (pit_sleep() needs the timer IRQ actually firing to make progress). */
void smp_init(void);

#endif
