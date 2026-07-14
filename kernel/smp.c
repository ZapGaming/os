#include <kernel/smp.h>
#include <kernel/acpi.h>
#include <kernel/apic.h>
#include <kernel/pit.h>
#include <kernel/serial.h>

void smp_init(void) {
    struct acpi_cpu_info cpus;
    if (!acpi_find_cpus(&cpus)) {
        serial_printf("smp: no usable ACPI/MADT info -- single-CPU boot, skipping AP bring-up\n");
        return;
    }

    if (cpus.cpu_count <= 1) {
        serial_printf("smp: MADT reports only %u CPU(s) -- nothing to wake, skipping AP bring-up\n",
                      cpus.cpu_count);
        return;
    }

    apic_init(cpus.lapic_phys_addr);
    uint32_t bsp_id = apic_current_id();

    uint8_t ap_id = 0;
    int found_ap = 0;
    for (uint32_t i = 0; i < cpus.cpu_count; i++) {
        if (cpus.apic_ids[i] != bsp_id) {
            ap_id = cpus.apic_ids[i];
            found_ap = 1;
            break;
        }
    }
    if (!found_ap) {
        serial_printf("smp: could not find a CPU id different from the BSP's own (%u) -- skipping\n", bsp_id);
        return;
    }

    serial_printf("smp: waking one AP (apic id=%u; BSP is id=%u; %u CPU(s) total reported) -- "
                  "it will bring itself up as a real second scheduler participant "
                  "(kernel/apic.c's ap_main()); waking more than one AP is still explicit "
                  "follow-on work, not attempted by this pass\n",
                  ap_id, bsp_id, cpus.cpu_count);

    if (!apic_start_ap(ap_id)) {
        serial_printf("smp: AP bring-up failed -- continuing single-CPU; this is a soft failure, not a boot failure\n");
        return;
    }

    /* --- Proof it's genuinely running, not just that SIPI was sent ---
     * Deliberately temporary and easy to remove/build on: sample the
     * AP's heartbeat counter, let a couple of seconds pass with the
     * timer interrupt (and this BSP's own existing single-core
     * scheduler) running exactly as it always has, then sample again
     * and show it moved. This is the only place outside of
     * kernel/apic.c that ever reads the AP's state, and it only ever
     * reads -- never writes -- anything the AP touches. */
    uint32_t h0 = apic_ap_heartbeat();
    serial_printf("smp: AP heartbeat = %u, waiting ~2s to observe it moving on its own...\n", h0);
    pit_sleep(2000);
    uint32_t h1 = apic_ap_heartbeat();
    serial_printf("smp: AP heartbeat = %u after ~2s (%s)\n",
                  h1, (h1 > h0) ? "AP is genuinely running independently" : "DID NOT MOVE -- investigate");
}
