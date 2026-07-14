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
#include <kernel/object.h>
#include <kernel/reactor.h>
#include <kernel/registry.h>
#include <kernel/update.h>
#include <kernel/session.h>
#include <net/net.h>
#include <net/dhcp.h>
#include <drivers/ata.h>
#include <drivers/ac97.h>
#include <fs/fat32.h>
#include <stdint.h>

static volatile uint32_t bg_counter;
static int root_session_id;

uint32_t get_bg_counter(void) { return bg_counter; }

static void bg_task_entry(void) {
    uint32_t last_maintenance = 0;
    for (;;) {
        bg_counter++;
        uint32_t now = pit_ticks();
        if (now - last_maintenance >= 25) {
            signal_dispatch_scheduler();
            service_poll();
            reactor_tick();
            watchdog_kick("kernel.scheduler");
            watchdog_kick("nova.services");
            watchdog_poll();
            service_heartbeat("kernel-core");
            if (root_session_id > 0) session_touch(root_session_id);
            last_maintenance = now;
        }
    }
}

static void seed_registry(void) {
    int tx = registry_begin();
    if (tx < 0) return;
    registry_set(tx, "system.name", REG_STRING, "ZapOS Nova");
    registry_set(tx, "system.channel", REG_STRING, "experimental");
    registry_set(tx, "desktop.shell", REG_STRING, "nova");
    registry_set(tx, "security.sessionIsolation", REG_BOOL, "true");
    registry_set(tx, "updates.rollback", REG_BOOL, "true");
    registry_commit(tx);
}

static void init_platform_services(int net_up, int fs_up, int audio_up) {
    service_manager_init();
    service_register("kernel-core", "kernel.core", SERVICE_RESTART_NEVER);
    service_register("virtual-filesystem", "kernel.vfs", SERVICE_RESTART_ON_FAILURE);
    service_register("event-reactor", "kernel.reactor", SERVICE_RESTART_ON_FAILURE);
    service_register("security-session", "security.session", SERVICE_RESTART_ON_FAILURE);
    service_register("update-engine", "system.update", SERVICE_RESTART_ON_FAILURE);
    service_register("network-stack", "kernel.net", SERVICE_RESTART_ON_FAILURE);
    service_register("audio-stack", "kernel.audio", SERVICE_RESTART_ON_FAILURE);
    service_register("nova-desktop", "desktop.nova", SERVICE_RESTART_ON_FAILURE);
    service_add_dependency("virtual-filesystem", "kernel-core");
    service_add_dependency("event-reactor", "kernel-core");
    service_add_dependency("security-session", "event-reactor");
    service_add_dependency("update-engine", "virtual-filesystem");
    service_add_dependency("network-stack", "kernel-core");
    service_add_dependency("audio-stack", "kernel-core");
    service_add_dependency("nova-desktop", "virtual-filesystem");
    service_add_dependency("nova-desktop", "security-session");
    service_start("kernel-core", 0);
    service_start("virtual-filesystem", 0);
    service_start("event-reactor", 0);
    service_start("security-session", 0);
    service_start("update-engine", 0);
    if (net_up) service_start("network-stack", 0);
    if (audio_up) service_start("audio-stack", 0);

    watchdog_register("kernel.scheduler", 300, 1);
    watchdog_register("nova.services", 500, 1);
    watchdog_register("event.reactor", 500, 1);
    watchdog_register("session.security", 1000, 1);
    watchdog_register("network.stack", 1000, 0);
    watchdog_register("storage.vfs", 1000, 0);
    watchdog_kick("kernel.scheduler");
    watchdog_kick("nova.services");
    watchdog_kick("event.reactor");
    watchdog_kick("session.security");
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
    object_manager_init();
    reactor_init();
    registry_init();
    update_manager_init();
    session_manager_init();
    seed_registry();

    ipc_register(0, "kernel.core");
    object_create(OBJECT_PROCESS, 0, "kernel-main", RIGHT_READ | RIGHT_SIGNAL | RIGHT_DUP | RIGHT_ADMIN);
    object_create(OBJECT_SERVICE, 0, "kernel.core", RIGHT_READ | RIGHT_SIGNAL | RIGHT_ADMIN);
    object_create(OBJECT_CHANNEL, 0, "system-events", RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL | RIGHT_DUP);
    reactor_watch(0, REACTOR_TIMER, 1, 100);
    struct user_session root_session;
    root_session_id = session_open(1, 0, &root_session);
    init_platform_services(net_up, fs_up, audio_up);

    nova_event_emit(NOVA_SUCCESS, "architecture", "Kernel object handles and rights online");
    nova_event_emit(NOVA_SUCCESS, "architecture", "Asynchronous event reactor online");
    nova_event_emit(NOVA_SUCCESS, "architecture", "Transactional system registry committed");
    nova_event_emit(NOVA_SUCCESS, "architecture", "Staged update and rollback engine online");
    nova_event_emit(root_session_id > 0 ? NOVA_SUCCESS : NOVA_ERROR, "security", root_session_id > 0 ? "Root session isolation active" : "Session initialization failed");
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
