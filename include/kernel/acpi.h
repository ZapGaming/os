#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stdint.h>

/* Generous upper bound for a hobby-OS MVP -- this pass only ever wakes
 * one AP no matter how many the MADT reports (see kernel/smp.c), this
 * just needs to be big enough that acpi_find_cpus() can enumerate every
 * CPU QEMU is configured with (-smp N) without truncating the list. */
#define ACPI_MAX_CPUS 16

struct acpi_cpu_info {
    /* Physical address of the Local APIC MMIO region, from the MADT's
     * "Local Interrupt Controller Address" field -- typically
     * 0xFEE00000. Valid only if acpi_find_cpus() returned 1. */
    uint32_t lapic_phys_addr;

    /* Number of ENABLED Processor Local APIC entries found (MADT entry
     * type 0, flags bit 0 set). This is "every CPU QEMU reports" per
     * the task brief -- includes the BSP itself, which the caller must
     * filter out before picking a target for AP wake-up (see
     * kernel/smp.c, which compares against apic_current_id()). */
    uint32_t cpu_count;
    uint8_t  apic_ids[ACPI_MAX_CPUS];
};

/* Locates the RSDP (searching the first 1KB of the EBDA and the BIOS
 * area 0xE0000-0xFFFFF, per the ACPI spec's documented search
 * algorithm), validates it, walks RSDT/XSDT to find the MADT ("APIC")
 * table, and fills *out with the Local APIC MMIO base address and every
 * enabled CPU's APIC ID.
 *
 * Returns 1 on success (a valid MADT was found -- *out is populated,
 * though cpu_count may still be 1 on a genuinely single-CPU MADT).
 * Returns 0 if no RSDP/MADT could be found at all, or either table
 * failed its checksum -- callers MUST treat 0 as "no usable ACPI info,
 * assume single-CPU and do not attempt AP bring-up", not as an error
 * worth failing the boot over. This function never hangs or crashes:
 * every scan is bounded, and every table access is checksum-validated
 * before being trusted. */
int acpi_find_cpus(struct acpi_cpu_info *out);

#endif
