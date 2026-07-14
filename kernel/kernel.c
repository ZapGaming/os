#include <kernel/serial.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/pit.h>
#include <kernel/multiboot.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>
#include <kernel/kheap.h>
#include <drivers/ps2.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <gui/framebuffer.h>
#include <gui/compositor.h>
#include <kernel/scheduler.h>
#include <kernel/fpu.h>
#include <kernel/demo.h>
#include <kernel/exceptions.h>
#include <kernel/tss.h>
#include <kernel/syscall.h>
#include <kernel/demo_user_task.h>
#include <kernel/ping_task.h>
#include <kernel/smp.h>
#include <kernel/apic.h>
#include <net/net.h>
#include <net/dhcp.h>
#include <drivers/ata.h>
#include <drivers/ac97.h>
#include <fs/fat32.h>
#include <stdint.h>
#include <string.h>

/* May run on the BSP (single-CPU boot, or if AP bring-up failed) or on
 * the AP (see kernel_main()'s re-pin right after smp_init()) -- either
 * way, only ONE core is ever the one incrementing it at a time
 * (kernel/scheduler.c's cpu_affinity partitioning guarantees a given
 * task never runs on two cores simultaneously), so this stays a safe
 * single-writer volatile counter exactly like it always was; reading it
 * from get_bg_counter() (the GUI, on the BSP) needs no lock for the same
 * reason apic_ap_heartbeat() doesn't. */
static volatile uint32_t bg_counter = 0;

uint32_t get_bg_counter(void) {
    return bg_counter;
}

static void bg_task_entry(void) {
    for (;;) {
        bg_counter++;
    }
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    serial_init();
    serial_printf("ZapOS booting...\n");
    serial_printf("multiboot magic=%x info=%x\n", magic, mb_info_addr);

    gdt_init();
    serial_printf("GDT loaded\n");

    tss_init();
    serial_printf("TSS loaded\n");

    idt_init();
    serial_printf("IDT loaded\n");

    exceptions_init();
    serial_printf("Exception handlers registered\n");

    syscall_init();

    pic_remap();
    serial_printf("PIC remapped\n");

    pit_init(100);
    serial_printf("PIT initialized\n");

    fpu_init();
    serial_printf("FPU enabled\n");

    struct mb_parsed_info mb_info;
    multiboot_parse(mb_info_addr, &mb_info);

    paging_init();
    pmm_init(&mb_info);
    kheap_init();

    ps2_init();
    keyboard_init();
    mouse_init();

    /* Unmask every IRQ line this kernel actually drives at the PIC
     * itself, rather than trusting whatever mask pic_remap() inherited
     * from the BIOS/bootloader handoff -- the same class of bug fixed
     * in ata.c's ATA_STATUS_FLOATING (something that happened to work
     * under one BIOS/emulator's default mask isn't guaranteed under
     * another). Concretely: IRQ1 was found masked on some boot paths,
     * which silently ate all keyboard input with no error, just a
     * text box that never responds to typing. IRQ2 is the master
     * PIC's cascade line to the slave -- required for ANY slave IRQ
     * (12, the mouse, here) to ever reach the CPU regardless of the
     * slave's own mask bit. */
    pic_clear_mask(0);  /* PIT timer */
    pic_clear_mask(1);  /* PS/2 keyboard */
    pic_clear_mask(2);  /* cascade to slave PIC */
    pic_clear_mask(12); /* PS/2 mouse */

    if (mb_info.has_framebuffer) {
        mouse_set_bounds(mb_info.fb_width, mb_info.fb_height);
        fb_init((uint32_t)mb_info.fb_addr, mb_info.fb_pitch,
                 mb_info.fb_width, mb_info.fb_height, mb_info.fb_bpp);
    }

    int net_up = net_init();
    if (net_up) serial_printf("Network: %s up\n", net_get_driver_name());
    else serial_printf("Network: no NIC found\n");

    int disk_ready = ata_init();
    if (!disk_ready && mb_info.has_module) {
        /* No real ATA hard disk (e.g. booted from the ISO alone, with
         * no second -drive) -- fall back to the FAT32 image GRUB
         * loaded as a module (see iso/grub.cfg), so the filesystem
         * (and DOOM.ELF etc. on it) is still there either way. */
        ata_use_ram_disk((void *)mb_info.module_addr, mb_info.module_size);
        disk_ready = 1;
    }
    int fs_up = disk_ready && fat32_init();
    serial_printf(fs_up ? "Filesystem: FAT32 mounted\n" : "Filesystem: no disk/FAT32 found\n");

    int audio_up = ac97_init();
    serial_printf(audio_up ? "Audio: AC97 ready\n" : "Audio: no codec found\n");

    scheduler_init();
    /* Kept exactly where it always was, pinned to its default
     * (SCHED_CPU_BSP) affinity for now -- see the re-pin right after
     * smp_init(), below, for why and when this moves to the AP. */
    struct task *bg_task = task_create(bg_task_entry);
    task_create_user(demo_user_task_entry);
    if (net_up) task_create(ping_task_entry);
    pit_set_tick_callback(schedule);
    scheduler_start();
    serial_printf("Scheduler started with %d tasks\n", scheduler_task_count());

    __asm__ volatile ("sti");
    serial_printf("Interrupts enabled\n");

    /* SMP bring-up: detects every CPU via ACPI/MADT and, only if more
     * than one is reported, wakes exactly one AP -- which now (see
     * kernel/apic.c's ap_main()) brings itself all the way up into a
     * real, lock-protected second scheduler participant, not just a
     * parked heartbeat loop. Still a clean no-op on a single-CPU boot
     * (the default for this kernel/QEMU invocation unless run with
     * `-smp 2`). Needs interrupts on (pit_sleep(), used both inside
     * apic_send_init_sipi()'s timing and smp_init()'s own proof-of-life
     * wait, and now also apic_start_periodic_timer()'s calibration --
     * needs the timer IRQ actually firing to make progress), so it
     * can't run any earlier than this. See include/kernel/smp.h for the
     * current full scope-cut list. */
    smp_init();

    /* Real cross-core scheduling proof-of-life for this pass: if (and
     * only if) an AP actually came up -- smp_init() above already
     * blocked long enough to prove that with a heartbeat-counter check
     * of its own -- re-pin the pre-existing bg_task to it, so it's the
     * BSP and the AP genuinely round-robin-scheduling two independent
     * things at once (bg_task on the AP; everything else -- GUI,
     * ping_task, demo_user_task -- still exclusively on the BSP).
     * bg_task_entry only ever touches its own `bg_counter` (a single
     * volatile uint32_t, kernel/kernel.c) -- no kmalloc/kfree, no GUI,
     * no network, no filesystem -- which is exactly the "deliberately
     * shared-state-free beyond the scheduler's own lock" property this
     * pass requires of anything it schedules onto the AP. If no AP came
     * up (single-CPU boot, or bring-up failed), bg_task simply stays on
     * SCHED_CPU_BSP -- its default -- which is byte-for-byte this
     * kernel's behavior before this pass. task_set_cpu_affinity() takes
     * kernel/scheduler.c's own lock, so this is safe to call here even
     * though interrupts are already enabled and the BSP's scheduler is
     * already preempting concurrently. */
    if (bg_task && apic_ap_started_count() > 0) {
        task_set_cpu_affinity(bg_task, SCHED_CPU_AP);
        serial_printf("kernel: bg counter task re-pinned to the AP now that it's up\n");
    }

    /* Needs interrupts on (it blocks waiting for replies, same as
     * dns_resolve()) so it can't run any earlier than this -- net_init()
     * already gave every network consumer a usable static fallback
     * config, so this is a best-effort upgrade, not something anything
     * else needs to wait for. ping_task (already created above, and
     * about to start competing for CPU time via preemption) re-reads
     * the gateway fresh every iteration rather than caching it, so it
     * can't observe a permanently-stale pre-DHCP value either way. */
    if (net_up) net_dhcp_negotiate();

    gui_init();
    serial_printf("Entering GUI main loop\n");
    gui_run();
}
