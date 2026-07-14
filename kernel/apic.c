/* Local APIC driver -- register-level access, software enable, the
 * INIT-SIPI-SIPI sequence used to wake exactly one AP, and (new this
 * pass) that AP's own per-core GDT/IDT/TSS/timer bring-up and real
 * scheduler participation. See include/kernel/smp.h for the
 * orchestration (ACPI lookup + calling into this file) and the full
 * list of things this pass still deliberately does NOT do. */
#include <kernel/apic.h>
#include <kernel/pmm.h>
#include <kernel/kheap.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/tss.h>
#include <kernel/scheduler.h>
#include <string.h>

/* Local APIC register byte offsets from the MMIO base (Intel SDM Vol.3
 * Ch.10, "Advanced Programmable Interrupt Controller"). Only the
 * handful this pass actually needs. */
#define APIC_REG_ID          0x020u
#define APIC_REG_EOI         0x0B0u
#define APIC_REG_SVR         0x0F0u
#define APIC_REG_ICR_LOW     0x300u
#define APIC_REG_ICR_HIGH    0x310u
#define APIC_REG_LVT_TIMER   0x320u
#define APIC_REG_TIMER_ICR   0x380u /* initial count */
#define APIC_REG_TIMER_CCR   0x390u /* current count, counts down from ICR */
#define APIC_REG_TIMER_DCR   0x3E0u /* divide configuration */

#define APIC_SVR_ENABLE          (1u << 8)
#define APIC_ICR_DELIVERY_STATUS (1u << 12)

#define APIC_LVT_MASKED          (1u << 16)
#define APIC_LVT_TIMER_PERIODIC  (1u << 17)

/* This pass's AP-local-timer interrupt vector -- outside the CPU
 * exception range (0-31), outside the legacy PIC's remapped IRQ range
 * (32-47, kernel/pic.c), and distinct from the syscall gate (0x80,
 * kernel/syscall.c). Its IDT gate is installed directly by this file
 * (apic_init(), below) rather than by kernel/idt.c's idt_init() --
 * exactly the same pattern kernel/syscall.c already uses for 0x80. */
#define AP_TIMER_VECTOR 80u /* 0x50 */

/* ICR low-dword values for the classic legacy MP-init wake sequence
 * (Intel SDM Vol.3 Ch.10.6, "MP Initialization Protocol Algorithm").
 * Destination is always given explicitly via ICR_HIGH bits 24-31
 * (physical destination mode -- ICR_LOW bit 11 stays 0), so no
 * destination-shorthand bits are ever needed here. */
#define ICR_INIT_ASSERT   0x00004500u /* delivery mode 101 (INIT) | level assert   (bit14) */
#define ICR_INIT_DEASSERT 0x00008500u /* delivery mode 101 (INIT) | trigger mode level (bit15), level bit clear = deassert */
#define ICR_STARTUP       0x00004600u /* delivery mode 110 (Startup) | level assert (bit14); vector OR'd in by the caller */

static volatile uint32_t *lapic_base = NULL;

/* Recorded once each, used only by apic_cpu_index() -- see that
 * function and include/kernel/apic.h's doc comment on it. bsp_apic_id
 * is set inside apic_init() (always the BSP, per kernel/smp.c); the
 * ap_apic_id_valid guard is what makes apic_cpu_index() correctly say
 * "BSP" for every call before any AP has actually been targeted, rather
 * than comparing against a stale 0. */
static uint32_t bsp_apic_id = 0;
static uint8_t  ap_apic_id = 0;
static int      ap_apic_id_valid = 0;

extern void isr80(void); /* boot/../kernel/isr_stubs.asm's ISR_NOERR 80 */
static void apic_timer_isr(struct registers *regs); /* defined below, near apic_start_periodic_timer() */

static uint32_t apic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void apic_write(uint32_t reg, uint32_t value) {
    lapic_base[reg / 4] = value;
}

/* The actual "flip the software-enable bit + program a valid spurious
 * vector" sequence -- factored out so both apic_init() (BSP, once) and
 * apic_enable_this_cpu() (the AP, once, from ap_main()) can share it
 * without either re-deriving lapic_phys_addr or re-logging apic_init()'s
 * own boot-time message. */
static void apic_software_enable(void) {
    uint32_t svr = apic_read(APIC_REG_SVR);
    svr |= APIC_SVR_ENABLE;
    /* Spurious vector 0xFF: outside the PIC's remapped 0x20-0x2F IRQ
     * range (kernel/pic.c) and this pass's own AP_TIMER_VECTOR (80),
     * and nothing ever unmasks anything that would cause it to actually
     * fire -- the hardware just requires *some* valid vector programmed
     * here regardless. */
    svr = (svr & 0xFFFFFF00u) | 0xFFu;
    apic_write(APIC_REG_SVR, svr);
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

    apic_software_enable();
    bsp_apic_id = apic_current_id();

    /* Installed here, once, unconditionally -- harmless on a
     * single-CPU boot (nothing ever unmasks an LVT Timer to use this
     * vector, so it can never fire), and required before the AP could
     * ever safely unmask its own LVT Timer against it. Same pattern
     * kernel/syscall.c already uses for its own vector (0x80): a gate
     * installed directly by the subsystem that owns it, not by
     * kernel/idt.c's central idt_init(). */
    idt_set_gate(AP_TIMER_VECTOR, (uint32_t)isr80, 0x08, 0x8E);
    register_interrupt_handler(AP_TIMER_VECTOR, apic_timer_isr);

    serial_printf("apic: Local APIC at %x software-enabled (this CPU's id=%u, recorded as the BSP)\n",
                  lapic_phys_addr, bsp_apic_id);
}

void apic_enable_this_cpu(void) {
    /* lapic_base must already be set by the BSP's apic_init() call --
     * see include/kernel/apic.h's doc comment on why reusing that same
     * pointer value here, from a different core, is correct rather than
     * a mistake. */
    apic_software_enable();
    serial_printf("apic: Local APIC software-enabled on this CPU (id=%u)\n", apic_current_id());
}

uint32_t apic_cpu_index(void) {
    if (!lapic_base) return SCHED_CPU_BSP; /* apic_init() never ran -- single-CPU boot */
    if (ap_apic_id_valid && apic_current_id() == ap_apic_id) return SCHED_CPU_AP;
    return SCHED_CPU_BSP;
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

/* --- AP-local timer -------------------------------------------------- */

/* Sends this core's own Local APIC EOI -- required for any
 * LAPIC-delivered interrupt (unlike a PIC-delivered one, which
 * kernel/idt.c's isr_dispatch() already EOIs on the PIC itself for
 * int_no 32-47; AP_TIMER_VECTOR, 80, is outside that range, so
 * isr_dispatch() correctly leaves EOI-ing it to this handler). Without
 * this, the LAPIC would consider the timer interrupt permanently "in
 * service" and never deliver another one. */
static void apic_send_eoi(void) {
    apic_write(APIC_REG_EOI, 0);
}

/* Registered once (from apic_init(), on the BSP, before the AP exists)
 * against AP_TIMER_VECTOR. In practice only ever actually fires on the
 * AP -- the BSP never configures/unmasks its own LVT Timer (it keeps
 * using the legacy PIT/IRQ0 for schedule(), exactly as before this
 * pass) -- but there's nothing BSP-specific in the body itself: EOI
 * this core's own LAPIC, then let kernel/scheduler.c's schedule() (which
 * is itself already per-CPU-aware via apic_cpu_index()) do the rest. */
static void apic_timer_isr(struct registers *regs) {
    (void)regs;
    apic_send_eoi();
    schedule();
}

void apic_start_periodic_timer(void) {
    apic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED); /* stay masked while we calibrate */
    apic_write(APIC_REG_TIMER_DCR, 0x3);              /* divide by 16 */

    /* Rough calibration against the BSP-driven PIT: run this core's
     * timer down from its max count for ~5 PIT ticks (~50ms at the
     * 100Hz kernel/pit.c is configured for), see how far it got, and
     * derive a periodic initial-count from that. pit_ticks() is a
     * single-writer (the BSP's own IRQ0 handler) volatile counter --
     * safe to read from here without a lock, same reasoning as
     * apic_ap_heartbeat() elsewhere in this file. */
    uint32_t start_tick = pit_ticks();
    apic_write(APIC_REG_TIMER_ICR, 0xFFFFFFFFu);
    while (pit_ticks() - start_tick < 5u) {
        __asm__ volatile ("pause");
    }
    uint32_t elapsed_counts = 0xFFFFFFFFu - apic_read(APIC_REG_TIMER_CCR);

    uint32_t counts_per_period = elapsed_counts / 5u; /* ~10ms worth, i.e. roughly the BSP's own PIT period */
    if (counts_per_period == 0) counts_per_period = 1000000u; /* paranoia fallback -- should never trigger under QEMU */

    apic_write(APIC_REG_LVT_TIMER, AP_TIMER_VECTOR | APIC_LVT_TIMER_PERIODIC);
    apic_write(APIC_REG_TIMER_ICR, counts_per_period); /* also (re)starts the counter */

    serial_printf("apic: this CPU's (id=%u) Local APIC timer calibrated (~%u counts per ~10ms) "
                  "and started in periodic mode on vector %u\n",
                  apic_current_id(), counts_per_period, AP_TIMER_VECTOR);
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
 * `ap_entry_ptr` -- NOT through kernel_main() (which only ever runs on
 * the BSP). Starts out under the trampoline's own tiny temporary flat
 * GDT (boot/ap_trampoline.asm), just long enough to reach 32-bit
 * protected mode; the very first thing this function does is leave
 * that behind for the kernel's REAL, shared GDT/IDT/TSS -- see the
 * per-CPU bring-up sequence below, and note that this is exactly the
 * "per-CPU GDT/TSS separation" the prior pass's comments here called
 * explicit follow-on work.
 *
 * Ordering below is load-bearing, not stylistic:
 *   1. gdt_load_this_cpu()   -- LGDT: needed before LTR can resolve a
 *      TSS selector against it.
 *   2. tss_load_ap()          -- LTR: this core's own esp0 now lives in
 *      tss[1] (kernel/tss.c), not the BSP's tss[0].
 *   3. idt_load_this_cpu()    -- LIDT: needed before this core can take
 *      ANY interrupt safely (including the timer this function is
 *      about to configure).
 *   4. apic_enable_this_cpu() -- software-enables this core's OWN Local
 *      APIC (a per-core SVR bit -- apic_init() enabling the BSP's did
 *      nothing for this one).
 *   5. scheduler_init_ap()    -- registers THIS call stack as a real,
 *      schedulable struct task pinned to SCHED_CPU_AP, and makes it
 *      this CPU's current task -- MUST happen before step 6 unmasks
 *      the timer that could otherwise call schedule() against a still-
 *      NULL current_task_cpu[SCHED_CPU_AP].
 *   6. apic_start_periodic_timer() -- calibrates and unmasks this
 *      core's own timer interrupt. Configuring/unmasking it here, before
 *      `sti` below, is still safe: a masked-for-interrupt-delivery
 *      condition (IF=0) just holds the first tick pending, exactly like
 *      any other maskable interrupt -- it isn't lost, and it can't fire
 *      before step 7 anyway.
 *   7. sti                    -- only now does this core actually start
 *      taking interrupts.
 * From here on, the loop below IS this CPU's "task 0" (mirroring how
 * kernel_main()'s own call stack is tasks[0] for the BSP -- see
 * scheduler_init()): scheduler_init_ap() already made it a real,
 * preemptible task, so kernel/scheduler.c's schedule() (driven by this
 * core's own Local APIC timer, never the legacy PIT/IRQ0 the BSP still
 * uses) can now genuinely time-share this core between it and whatever
 * else kernel/kernel.c pinned to SCHED_CPU_AP -- see that file for
 * exactly which existing task that is, and why only that one. */
void ap_main(void) {
    ap_started_count++;

    /* Best-effort only, and known-unsafe under real concurrency:
     * serial_printf()/serial_putc() (kernel/serial.c) poll one shared
     * UART with no lock of any kind -- deliberately NOT fixed by this
     * pass either (see the task-level scope cut; kernel/spinlock.h's
     * new lock is used for the scheduler's shared state, not this). A
     * byte written here could in principle interleave with whatever the
     * BSP happens to be printing at the same instant. Used here, and in
     * the per-CPU bring-up functions below, purely as diagnostic
     * breadcrumbs -- never anything this kernel's correctness depends
     * on. */
    serial_printf("apic: AP is alive and running independently of the BSP\n");

    gdt_load_this_cpu();
    tss_load_ap();
    idt_load_this_cpu();
    apic_enable_this_cpu();
    scheduler_init_ap();
    apic_start_periodic_timer();

    __asm__ volatile ("sti");
    serial_printf("apic: AP's per-CPU bring-up complete -- GDT/IDT/TSS loaded, own LAPIC timer running, interrupts on\n");

    for (;;) {
        for (volatile uint32_t i = 0; i < 2000000u; i++) { }
        ap_heartbeat++;
    }
}

int apic_start_ap(uint8_t apic_id) {
    /* Recorded before anything else -- see include/kernel/apic.h's doc
     * comment on apic_cpu_index(), which this makes meaningful. Safe to
     * set even if everything below this point fails and bring-up is
     * aborted: apic_cpu_index() only ever returns SCHED_CPU_AP for a
     * Local APIC ID that has actually been targeted by a SIPI, and if
     * that AP never actually starts executing (ap_started_count stays
     * 0), nothing ever calls apic_cpu_index() from that core to notice. */
    ap_apic_id = apic_id;
    ap_apic_id_valid = 1;

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
