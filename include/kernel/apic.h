#ifndef KERNEL_APIC_H
#define KERNEL_APIC_H

#include <stdint.h>

/* Fixed physical load address for the AP trampoline (see
 * boot/ap_trampoline.asm, whose ORG directive MUST match this value,
 * and kernel/pmm.c's pmm_alloc_low_frame(), which this gets passed to).
 * Below 1MB and 4KB-page-aligned, as the x86 INIT-SIPI-SIPI protocol
 * requires -- 0x8000 (32KB) sits well clear of the IVT/BDA (0-0x1000)
 * and any realistic EBDA, and nothing else in this kernel ever uses
 * sub-1MB physical memory for anything (the kernel image itself starts
 * at 1MB -- see linker.ld's ". = 1M"). */
#define AP_TRAMPOLINE_PHYS_ADDR 0x8000u

/* Software-enables the Local APIC at the given physical MMIO base
 * (typically 0xFEE00000, read from the MADT -- see kernel/acpi.h), and
 * installs the gate + handler for this pass's AP-local-timer vector
 * (see AP_TIMER_VECTOR below). Called exactly once, by the BSP, from
 * kernel/smp.c -- this is also where the BSP's own Local APIC ID gets
 * recorded (see apic_cpu_index() below).
 *
 * Scope cut: this does NOT route any external/IOAPIC interrupt through
 * the APIC and does NOT touch the PIC at all -- the 8259 (kernel/pic.c)
 * stays the BSP's sole interrupt source (the AP uses its own Local APIC
 * timer instead -- see apic_start_periodic_timer() -- never the PIC,
 * which this kernel's interrupt routing never delivers to the AP
 * anyway). The spurious interrupt vector is configured because the
 * hardware requires a valid value there, not because this kernel
 * expects it to ever fire. */
void apic_init(uint32_t lapic_phys_addr);

/* This CPU's own Local APIC ID (ID register bits 24-31). Only valid
 * after apic_init() has run on that CPU (the BSP; see apic_init()) or,
 * for the AP, after apic_enable_this_cpu() has run on it. */
uint32_t apic_current_id(void);

/* Software-enables THIS core's own Local APIC. Genuinely per-core state
 * -- the APIC Software Enable bit in the SVR lives inside each core's
 * own Local APIC unit, so apic_init() enabling the BSP's does nothing
 * for the AP's. Reuses the same lapic_base MMIO pointer apic_init()
 * already recorded: accessing the fixed Local APIC physical base
 * address is architecturally a per-core operation (every core's access
 * to that address is intercepted by its OWN Local APIC hardware, never
 * forwarded to another core's -- that's what "local" means here), so
 * the same pointer value is correct on every core. Called once, by the
 * AP itself, from ap_main(). */
void apic_enable_this_cpu(void);

/* Maps the calling core's own Local APIC ID to a small, stable index:
 * SCHED_CPU_BSP (0) for the BSP, SCHED_CPU_AP (1) for the one AP this
 * pass ever wakes (see kernel/scheduler.h). Safe to call from ANY
 * context, including a single-CPU boot where apic_init() never ran at
 * all -- in that case lapic_base is still NULL and this returns
 * SCHED_CPU_BSP immediately without touching any Local APIC MMIO
 * register, which is what makes it safe for kernel/scheduler.c to call
 * unconditionally from schedule()/scheduler_current()/task_exited() on
 * every single boot, SMP or not. This is the one small piece of
 * "orchestration" logic that lives in apic.c rather than smp.c/
 * scheduler.c, because it needs apic.c's own bsp/ap Local APIC ID
 * bookkeeping (recorded by apic_init() and apic_start_ap()) to answer
 * the question at all. */
uint32_t apic_cpu_index(void);

/* Roughly calibrates (against the BSP-driven PIT's known tick rate) and
 * starts THIS core's own Local APIC timer as a periodic interrupt
 * source on AP_TIMER_VECTOR, then unmasks it. LVT Timer/initial-count/
 * current-count/divide-configuration are genuinely per-core registers
 * (Intel SDM Vol.3 Ch.10.5) -- every core that wants its own timer
 * interrupts must call this itself. Deliberately approximate ("doesn't
 * need to be precise for this pass" -- see the task-level writeup);
 * good enough to drive kernel/scheduler.c's schedule() at a rate in the
 * same ballpark as the BSP's 100Hz PIT, not a real time source. Must be
 * called after apic_enable_this_cpu() (needs this core's LAPIC already
 * software-enabled) and, in practice, after scheduler_init_ap() (so
 * that the very first tick this unmasks always has somewhere valid to
 * switch to/from) -- see ap_main() for the exact ordering this and the
 * rest of the AP's per-CPU bring-up depends on. */
void apic_start_periodic_timer(void);

/* Sends the legacy INIT-SIPI-SIPI wake sequence to the CPU with the
 * given Local APIC ID, telling it to start executing in real mode at
 * trampoline_phys_addr (CS = addr>>4, IP = 0). The caller (see
 * apic_start_ap()) is responsible for the trampoline code already being
 * in place at that address before this is called. */
void apic_send_init_sipi(uint8_t apic_id, uint32_t trampoline_phys_addr);

/* High-level one-shot AP bring-up: allocates a dedicated stack for the
 * AP, copies+patches the trampoline blob (boot/ap_trampoline.asm) into
 * the low frame carved out via pmm_alloc_low_frame(), sends
 * INIT-SIPI-SIPI, and polls -- with a bounded timeout, this can never
 * hang the BSP forever -- for the AP to report in via
 * apic_ap_started_count(). Returns 1 on success, 0 on any failure
 * (out of memory, low frame already claimed, AP never reported in);
 * always safe to call and never crashes the BSP either way. */
int apic_start_ap(uint8_t apic_id);

/* Proof-of-life counters, incremented only by the AP itself (see
 * kernel/apic.c's ap_main()) and only ever read by the BSP through
 * these accessors. Reading a `volatile uint32_t` that only ever
 * increments is safe without a lock -- true of both of these,
 * unconditionally: apic_ap_started_count() is written exactly once, at
 * the very top of ap_main(), before any lock or shared structure this
 * pass adds even exists yet; ap_heartbeat is incremented from inside a
 * genuinely scheduled struct task (see scheduler_init_ap()), but still
 * only ever by whichever single core is currently running it, per the
 * scheduler's own cpu_affinity partitioning -- so both remain
 * single-writer even though, as of this pass, the AP is no longer
 * limited to touching only these two counters. (The prior pass's claim
 * here that these were "the ONLY shared mutable state the AP ever
 * touches" no longer holds -- see kernel/scheduler.c's sched_lock,
 * which is what makes the AP's now-much-larger footprint, the shared
 * tasks[]/task_count/next-chain, safe instead.) */
uint32_t apic_ap_started_count(void);
uint32_t apic_ap_heartbeat(void);

#endif
