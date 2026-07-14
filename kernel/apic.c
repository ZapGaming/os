/* Local APIC driver -- register-level access, software enable, and the
 * INIT-SIPI-SIPI sequence used to wake exactly one AP for this pass's
 * SMP bring-up MVP. See include/kernel/smp.h for the orchestration
 * (ACPI lookup + calling into this file) and the full list of things
 * this pass deliberately does NOT do. */
#include <kernel/apic.h>
#include <kernel/pmm.h>
#include <kernel/kheap.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <string.h>

/* Local APIC register byte offsets from the MMIO base (Intel SDM Vol.3
 * Ch.10, "Advanced Programmable Interrupt Controller"). Only the
 * handful this pass actually needs. */
#define APIC_REG_ID       0x020u
#define APIC_REG_SVR      0x0F0u
#define APIC_REG_ICR_LOW  0x300u
#define APIC_REG_ICR_HIGH 0x310u

#define APIC_SVR_ENABLE          (1u << 8)
#define APIC_ICR_DELIVERY_STATUS (1u << 12)

/* ICR low-dword values for the classic legacy MP-init wake sequence
 * (Intel SDM Vol.3 Ch.10.6, "MP Initialization Protocol Algorithm").
 * Destination is always given explicitly via ICR_HIGH bits 24-31
 * (physical destination mode -- ICR_LOW bit 11 stays 0), so no
 * destination-shorthand bits are ever needed here. */
#define ICR_INIT_ASSERT   0x00004500u /* delivery mode 101 (INIT) | level assert   (bit14) */
#define ICR_INIT_DEASSERT 0x00008500u /* delivery mode 101 (INIT) | trigger mode level (bit15), level bit clear = deassert */
#define ICR_STARTUP       0x00004600u /* delivery mode 110 (Startup) | level assert (bit14); vector OR'd in by the caller */

static volatile uint32_t *lapic_base = NULL;

static uint32_t apic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void apic_write(uint32_t reg, uint32_t value) {
    lapic_base[reg / 4] = value;
}

void apic_init(uint32_t lapic_phys_addr) {
    /* No explicit paging_map_*() call needed: paging_init()
     * (kernel/paging.c) already identity-maps the full 4GB physical
     * address space with 4MB pages before anything in kernel_main()
     * that could call this runs (its init loop covers all 1024
     * page-directory entries, i.e. 0..4GB, not a partial range), and
     * 0xFEE00000 -- the typical/QEMU Local APIC base -- falls inside
     * that range like any other physical address. */
    lapic_base = (volatile uint32_t *)lapic_phys_addr;

    uint32_t svr = apic_read(APIC_REG_SVR);
    svr |= APIC_SVR_ENABLE;
    /* Spurious vector 0xFF: outside the PIC's remapped 0x20-0x2F IRQ
     * range (kernel/pic.c), and this pass never unmasks anything that
     * would cause it to actually fire -- the hardware just requires
     * *some* valid vector programmed here regardless. */
    svr = (svr & 0xFFFFFF00u) | 0xFFu;
    apic_write(APIC_REG_SVR, svr);

    serial_printf("apic: Local APIC at %x software-enabled (this CPU's id=%u)\n",
                  lapic_phys_addr, apic_current_id());
}

uint32_t apic_current_id(void) {
    return (apic_read(APIC_REG_ID) >> 24) & 0xFFu;
}

static void apic_send_ipi(uint8_t apic_id, uint32_t icr_low) {
    apic_write(APIC_REG_ICR_HIGH, (uint32_t)apic_id << 24);
    apic_write(APIC_REG_ICR_LOW, icr_low);
    while (apic_read(APIC_REG_ICR_LOW) & APIC_ICR_DELIVERY_STATUS) { }
}

void apic_send_init_sipi(uint8_t apic_id, uint32_t trampoline_phys_addr) {
    uint8_t vector = (uint8_t)(trampoline_phys_addr >> 12);

    apic_send_ipi(apic_id, ICR_INIT_ASSERT);
    pit_sleep(10); /* Intel spec: wait >= 10ms after INIT before anything else */

    apic_send_ipi(apic_id, ICR_INIT_DEASSERT);

    for (int i = 0; i < 2; i++) {
        apic_send_ipi(apic_id, ICR_STARTUP | vector);
        pit_sleep(1); /* spec wants >=200us between/after SIPIs; one PIT tick (~10ms @100Hz) comfortably exceeds that */
    }
}

/* --- AP trampoline blob -------------------------------------------- */

/* Provided by boot/ap_trampoline_blob.asm, which incbin's the flat
 * binary assembled separately from boot/ap_trampoline.asm (see the
 * Makefile's dedicated `nasm -f bin` rule -- a normal `-f elf32` build
 * can't produce code that runs correctly at a fixed low real-mode
 * physical address). */
extern const uint8_t ap_trampoline_blob[];
extern const uint8_t ap_trampoline_blob_end[];

/* boot/ap_trampoline.asm's `ap_stack_top_ptr` and `ap_entry_ptr` are
 * deliberately the LAST two dwords in that file, patched here at
 * runtime rather than assembled in: the AP's stack address (a
 * kmalloc() result) and ap_main()'s link address aren't known until
 * the kernel is actually running. Do not reorder that file without
 * updating these two offsets. */
#define AP_STACK_PATCH_OFFSET_FROM_END 8u
#define AP_ENTRY_PATCH_OFFSET_FROM_END 4u

#define AP_STACK_SIZE (16u * 1024u)

static volatile uint32_t ap_started_count = 0;
static volatile uint32_t ap_heartbeat = 0;

uint32_t apic_ap_started_count(void) { return ap_started_count; }
uint32_t apic_ap_heartbeat(void) { return ap_heartbeat; }

/* The AP's entire C-level existence for this pass. Called directly by
 * boot/ap_trampoline.asm's 32-bit stub through the patched
 * `ap_entry_ptr` -- NOT through kernel_main(), scheduler_init(),
 * schedule(), or anything else that assumes single-core global state
 * (see include/kernel/smp.h's scope note; kernel/scheduler.c's
 * tasks[]/current_task are never touched from here or anywhere else in
 * this file). Runs forever under the trampoline's own tiny temporary
 * flat GDT (boot/ap_trampoline.asm) -- it deliberately never loads the
 * kernel's real GDT/TSS (kernel/gdt.c, kernel/tss.c), since a parked AP
 * that never enters ring 3 and never takes an interrupt needs neither;
 * per-CPU GDT/TSS separation is explicit follow-on work. */
void ap_main(void) {
    ap_started_count++;

    /* Best-effort only, and known-unsafe under real concurrency:
     * serial_printf()/serial_putc() (kernel/serial.c) poll one shared
     * UART with no lock of any kind -- this codebase has no spinlock or
     * atomic primitive anywhere to add one (confirmed by a repo-wide
     * grep before writing this). A byte written here could in
     * principle interleave with whatever the BSP happens to be
     * printing at the same instant. It's used here exactly once, at
     * wake, purely as a breadcrumb; a real second consumer of shared
     * kernel state would need a lock this codebase doesn't have yet. */
    serial_printf("apic: AP is alive and running independently of the BSP\n");

    for (;;) {
        for (volatile uint32_t i = 0; i < 2000000u; i++) { }
        ap_heartbeat++;
    }
}

int apic_start_ap(uint8_t apic_id) {
    uint8_t *stack = kmalloc(AP_STACK_SIZE);
    if (!stack) {
        serial_printf("apic: kmalloc(%u) for AP stack failed, aborting AP bring-up\n", AP_STACK_SIZE);
        return 0;
    }
    uint32_t stack_top = ((uint32_t)stack + AP_STACK_SIZE) & ~0xFu;

    uint32_t trampoline_phys = pmm_alloc_low_frame(AP_TRAMPOLINE_PHYS_ADDR);
    if (!trampoline_phys) {
        serial_printf("apic: could not claim the low trampoline frame, aborting AP bring-up\n");
        return 0;
    }

    uint32_t blob_size = (uint32_t)(ap_trampoline_blob_end - ap_trampoline_blob);
    memcpy((void *)trampoline_phys, ap_trampoline_blob, blob_size);

    uint32_t *stack_patch = (uint32_t *)(trampoline_phys + blob_size - AP_STACK_PATCH_OFFSET_FROM_END);
    uint32_t *entry_patch = (uint32_t *)(trampoline_phys + blob_size - AP_ENTRY_PATCH_OFFSET_FROM_END);
    *stack_patch = stack_top;
    *entry_patch = (uint32_t)ap_main;

    serial_printf("apic: trampoline (%u bytes) copied to %x, AP stack top=%x, entry=%x\n",
                  blob_size, trampoline_phys, stack_top, *entry_patch);

    apic_send_init_sipi(apic_id, trampoline_phys);

    uint32_t start_tick = pit_ticks();
    while (ap_started_count == 0 && (pit_ticks() - start_tick) < 100u) {
        __asm__ volatile ("pause");
    }

    if (ap_started_count == 0) {
        serial_printf("apic: AP %u did not report in within ~1s -- bring-up failed\n", apic_id);
        return 0;
    }

    serial_printf("apic: AP %u reported in (ap_started_count=%u)\n", apic_id, ap_started_count);
    return 1;
}
