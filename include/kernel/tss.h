#ifndef KERNEL_TSS_H
#define KERNEL_TSS_H

#include <stdint.h>

#define TSS_SELECTOR    0x28 /* BSP's TSS, GDT index 5 -- unchanged from the prior pass */
#define TSS_SELECTOR_AP 0x30 /* the AP's own TSS, GDT index 6 -- new this pass */

/* Capped at exactly 2 (BSP + one AP), matching kernel/smp.c only ever
 * waking one AP and kernel/scheduler.h's SCHED_MAX_CPUS. */
#define TSS_MAX_CPUS 2

void tss_init(void);

/* Loads TSS_SELECTOR_AP into TR -- must be called ONCE, by the AP
 * itself, as part of its per-CPU bring-up (kernel/apic.c's ap_main()),
 * after this CPU has already loaded the real GDT (gdt_load_this_cpu())
 * that selector resolves against. */
void tss_load_ap(void);

/* BSP-only convenience wrapper, kept for the one pre-existing call site
 * that only ever runs while executing AS the BSP; equivalent to
 * tss_set_kernel_stack_cpu(0, esp0). New code should prefer the
 * explicit per-CPU form below. */
void tss_set_kernel_stack(uint32_t esp0);

/* Sets esp0 in the TSS belonging to logical CPU `cpu_index` (0 = BSP,
 * 1 = the AP -- see kernel/scheduler.h's SCHED_CPU_BSP/SCHED_CPU_AP).
 * Needed because esp0 is genuinely per-CPU state now: once both cores
 * can take a ring0 transition (an interrupt, at minimum) concurrently,
 * one shared esp0 would let one core's interrupt clobber the stack
 * pointer the other core's transition just set up. Silently ignores an
 * out-of-range cpu_index rather than crashing -- this is called from
 * kernel/scheduler.c's schedule() on every task switch, which is not a
 * place to introduce a new way to take down the kernel over a bounds
 * bug. */
void tss_set_kernel_stack_cpu(int cpu_index, uint32_t esp0);

#endif
