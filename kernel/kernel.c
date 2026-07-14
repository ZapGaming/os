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
#include <kernel/nova.h>
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
    pic_clear_mask(0);
    pic_clear_mask(1);
    pic_clear_mask(2);
    pic_clear_mask(12);

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
        ata_use_ram_disk((void *)mb_info.module_addr, mb_info.module_size);
        disk_ready = 1;
    }
    int fs_up = disk_ready && fat32_init();
    serial_printf(fs_up ? "Filesystem: FAT32 mounted\n" : "Filesystem: no disk/FAT32 found\n");

    /* Nova is a kernel service rather than compositor-owned state. It starts
     * after FAT32 so preferences can be restored, but before the scheduler
     * and GUI so every later subsystem can publish events immediately. */
    nova_init();
    nova_event_emit(net_up ? NOVA_SUCCESS : NOVA_WARNING, "network",
                    net_up ? "Network interface initialized" : "No network interface detected");
    nova_event_emit(fs_up ? NOVA_SUCCESS : NOVA_WARNING, "storage",
                    fs_up ? "FAT32 workspace mounted" : "Running without persistent storage");

    int audio_up = ac97_init();
    serial_printf(audio_up ? "Audio: AC97 ready\n" : "Audio: no codec found\n");
    nova_event_emit(audio_up ? NOVA_SUCCESS : NOVA_INFO, "audio",
                    audio_up ? "AC97 audio online" : "Audio device unavailable");

    scheduler_init();
    struct task *bg_task = task_create(bg_task_entry);
    task_create_user(demo_user_task_entry);
    if (net_up) task_create(ping_task_entry);
    pit_set_tick_callback(schedule);
    scheduler_start();
    serial_printf("Scheduler started with %d tasks\n", scheduler_task_count());
    nova_event_emit(NOVA_SUCCESS, "scheduler", "Preemptive scheduler started");

    __asm__ volatile ("sti");
    serial_printf("Interrupts enabled\n");

    smp_init();

    if (bg_task && apic_ap_started_count() > 0) {
        task_set_cpu_affinity(bg_task, SCHED_CPU_AP);
        serial_printf("kernel: bg counter task re-pinned to the AP now that it's up\n");
        nova_event_emit(NOVA_SUCCESS, "smp", "Second CPU joined the scheduler");
        nova_notice_post(NOVA_SUCCESS, "Multi-core online", "Nova moved background work onto CPU 1.");
    } else {
        nova_event_emit(NOVA_INFO, "smp", "Single-core scheduling active");
    }

    if (net_up) {
        net_dhcp_negotiate();
        nova_event_emit(NOVA_INFO, "network", "DHCP negotiation completed");
    }

    gui_init();
    serial_printf("Entering GUI main loop\n");
    nova_event_emit(NOVA_SUCCESS, "desktop", "Nova desktop session started");
    gui_run();
}
