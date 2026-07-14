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
 * (typically 0xFEE00000, read from the MADT -- see kernel/acpi.h).
 *
 * Scope cut: this does NOT route any external/IOAPIC interrupt through
 * the APIC and does NOT touch the PIC at all -- the 8259 (kernel/pic.c)
 * stays the kernel's sole interrupt source for this pass. The spurious
 * interrupt vector is configured because the hardware requires a valid
 * value there, not because this kernel expects it to ever fire. */
void apic_init(uint32_t lapic_phys_addr);

/* This CPU's own Local APIC ID (ID register bits 24-31). Only valid
 * after apic_init() has run on that CPU. */
uint32_t apic_current_id(void);

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
 * increments is safe without a lock; these two counters (plus one
 * best-effort serial_printf at wake, documented on ap_main() itself)
 * are the ONLY shared mutable state this pass's AP code ever touches --
 * nothing resembling a linked structure, the scheduler's tasks[], or
 * any other multi-word shared state is ever read or written from the
 * AP side. */
uint32_t apic_ap_started_count(void);
uint32_t apic_ap_heartbeat(void);

#endif
