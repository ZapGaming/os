#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

/* Best-effort, safe-by-default entry point for this kernel's SMP
 * bring-up: detects CPUs via ACPI/MADT (kernel/acpi.c), and if -- and
 * only if -- more than one is reported, wakes exactly ONE additional
 * core (an "AP") via the Local APIC's INIT-SIPI-SIPI sequence
 * (kernel/apic.c), which -- as of this pass -- brings that AP all the
 * way up into a genuine, lock-protected, second participant in
 * kernel/scheduler.c's round-robin scheduler: its own GDT/IDT register
 * loads, its own TSS (kernel/tss.c), its own calibrated Local APIC
 * timer interrupt, and its own schedulable task(s) (see
 * kernel/kernel.c's bg_task re-pin) -- all of that per-CPU bring-up
 * lives in kernel/apic.c's ap_main(), not here; this function is just
 * ACPI lookup + calling apic_start_ap() + a proof-of-life delay. On a
 * single-CPU system, or if ACPI/MADT parsing fails for any reason, or
 * if AP bring-up itself fails, this is a clean no-op / soft-fail with
 * no behavior change from before SMP support existed -- it must NEVER
 * hang or crash a boot that would otherwise have worked.
 *
 * Explicit scope cuts still in force for this pass (see the task-level
 * writeup for the full reasoning) -- everything reachable from here
 * deliberately does NOT:
 *   - route PIC or IOAPIC interrupts through the APIC (the 8259 stays
 *     the BSP's sole interrupt source; the AP uses its own Local APIC
 *     timer instead, self-contained per-core, which needs none of
 *     that);
 *   - run GUI, network, filesystem, or ELF-loaded ("isolated") user
 *     code on the AP -- every task pinned to it is deliberately
 *     shared-state-free beyond what kernel/spinlock.h's new lock
 *     covers (see kernel/kernel.c);
 *   - wake more than one AP, or add any inter-core signaling (IPIs for
 *     rescheduling, TLB shootdown, etc.) beyond the one-time
 *     INIT-SIPI-SIPI wake itself;
 *   - make kernel/kheap.c's allocator, kernel/serial.c's UART, or any
 *     other shared subsystem besides the scheduler's own run queue
 *     SMP-safe -- kernel/spinlock.h's lock is used in exactly one
 *     place, kernel/scheduler.c's run queue, because that's the one
 *     piece of shared state this pass's AP task set actually touches
 *     concurrently with the BSP.
 * Call this once from kernel_main(), after interrupts are enabled
 * (pit_sleep() needs the timer IRQ actually firing to make progress,
 * and so -- now -- does apic_start_periodic_timer()'s calibration). */
void smp_init(void);

#endif
