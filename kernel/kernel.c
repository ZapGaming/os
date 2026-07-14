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
#include <kernel/capability.h>
#include <kernel/ipc.h>
#include <kernel/sysfs.h>
#include <kernel/vfs.h>
#include <kernel/service.h>
#include <kernel/signal.h>
#include <kernel/package.h>
#include <kernel/watchdog.h>
#include <net/net.h>
#include <net/dhcp.h>
#include <drivers/ata.h>
#include <drivers/ac97.h>
#include <fs/fat32.h>
#include <stdint.h>

static volatile uint32_t bg_counter;

uint32_t get_bg_counter(void) { return bg_counter; }

static void bg_task_entry(void) {
    uint32_t last_maintenance = 0;
    for (;;) {
        bg_counter++;
        uint32_t now = pit_ticks();
        if (now - last_maintenance >= 25) {
            signal_dispatch_scheduler();
            service_poll();
            watchdog_kick("kernel.scheduler");
            watchdog_kick("nova.services");
            watchdog_poll();
            service_heartbeat("kernel-core");
            last_maintenance = now;
        }
    }
}

static void init_platform_services(int net_up, int fs_up, int audio_up) {
    service_manager_init();
    service_register("kernel-core", "kernel.core", SERVICE_RESTART_NEVER);
    service_register("virtual-filesystem", "kernel.vfs", SERVICE_RESTART_ON_FAILURE);
    service_register("network-stack", "kernel.net", SERVICE_RESTART_ON_FAILURE);
    service_register("audio-stack", "kernel.audio", SERVICE_RESTART_ON_FAILURE);
    service_register("nova-desktop", "desktop.nova", SERVICE_RESTART_ON_FAILURE);
    service_add_dependency("virtual-filesystem", "kernel-core");
    service_add_dependency("network-stack", "kernel-core");
    service_add_dependency("audio-stack", "kernel-core");
    service_add_dependency("nova-desktop", "virtual-filesystem");
    service_start("kernel-core", 0);
    service_start("virtual-filesystem", 0);
    if (net_up) service_start("network-stack", 0);
    if (audio_up) service_start("audio-stack", 0);

    watchdog_register("kernel.scheduler", 300, 1);
    watchdog_register("nova.services", 500, 1);
    watchdog_register("network.stack", 1000, 0);
    watchdog_register("storage.vfs", 1000, 0);
    watchdog_kick("kernel.scheduler");
    watchdog_kick("nova.services");
    if (net_up) watchdog_kick("network.stack");
    if (fs_up) watchdog_kick("storage.vfs");
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    serial_init();
    serial_printf("ZapOS Nova platform booting...\n");
    serial_printf("multiboot magic=%x info=%x\n", magic, mb_info_addr);

    gdt_init();
    tss_init();
    idt_init();
    exceptions_init();
    syscall_init();
    pic_remap();
    pit_init(100);
    fpu_init();

    struct mb_parsed_info mb_info;
    multiboot_parse(mb_info_addr, &mb_info);
    paging_init();
    pmm_init(&mb_info);
    kheap_init();

    ps2_init();
    keyboard_init();
    mouse_init();
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
    int disk_ready = ata_init();
    if (!disk_ready && mb_info.has_module) {
        ata_use_ram_disk((void *)mb_info.module_addr, mb_info.module_size);
        disk_ready = 1;
    }
    int fs_up = disk_ready && fat32_init();
    int audio_up = ac97_init();

    nova_init();
    capability_init();
    ipc_init();
    sysfs_init();
    vfs_init();
    signal_init();
    package_manager_init();
    watchdog_init();
    ipc_register(0, "kernel.core");
    init_platform_services(net_up, fs_up, audio_up);

    nova_event_emit(NOVA_SUCCESS, "architecture", "Unified VFS mounted FAT32, procfs, sysfs and devfs");
    nova_event_emit(NOVA_SUCCESS, "architecture", "Dependency-aware service manager online");
    nova_event_emit(NOVA_SUCCESS, "architecture", "Queued process signal system online");
    nova_event_emit(NOVA_SUCCESS, "architecture", "Validated package registry online");
    nova_event_emit(NOVA_SUCCESS, "architecture", "Watchdog and recovery manager armed");
    nova_event_emit(net_up ? NOVA_SUCCESS : NOVA_WARNING, "network", net_up ? "Network interface initialized" : "No network interface detected");
    nova_event_emit(fs_up ? NOVA_SUCCESS : NOVA_WARNING, "storage", fs_up ? "FAT32 workspace mounted" : "Running without persistent storage");
    nova_event_emit(audio_up ? NOVA_SUCCESS : NOVA_INFO, "audio", audio_up ? "AC97 audio online" : "Audio device unavailable");

    scheduler_init();
    struct task *bg_task = task_create_named(bg_task_entry, "platform-supervisor");
    task_create_user_named(demo_user_task_entry, "ring3-demo");
    if (net_up) task_create_named(ping_task_entry, "network-ping");
    pit_set_tick_callback(schedule);
    scheduler_start();

    __asm__ volatile ("sti");
    smp_init();
    if (bg_task && apic_ap_started_count() > 0) {
        task_set_cpu_affinity(bg_task, SCHED_CPU_AP);
        nova_event_emit(NOVA_SUCCESS, "smp", "Platform supervisor moved to CPU 1");
    }

    if (net_up) {
        net_dhcp_negotiate();
        watchdog_kick("network.stack");
    }

    service_start("nova-desktop", 0);
    gui_init();
    nova_event_emit(NOVA_SUCCESS, "desktop", "Nova desktop session started");
    gui_run();
}
