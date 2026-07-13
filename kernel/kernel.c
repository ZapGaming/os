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
#include <kernel/demo.h>
#include <kernel/exceptions.h>
#include <kernel/tss.h>
#include <kernel/syscall.h>
#include <kernel/demo_user_task.h>
#include <kernel/ping_task.h>
#include <net/net.h>
#include <drivers/ata.h>
#include <drivers/ac97.h>
#include <fs/fat32.h>
#include <stdint.h>
#include <string.h>

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

    struct mb_parsed_info mb_info;
    multiboot_parse(mb_info_addr, &mb_info);

    paging_init();
    pmm_init(&mb_info);
    kheap_init();

    ps2_init();
    keyboard_init();
    mouse_init();
    if (mb_info.has_framebuffer) {
        mouse_set_bounds(mb_info.fb_width, mb_info.fb_height);
        fb_init((uint32_t)mb_info.fb_addr, mb_info.fb_pitch,
                 mb_info.fb_width, mb_info.fb_height, mb_info.fb_bpp);
    }

    int net_up = net_init();
    serial_printf(net_up ? "Network: rtl8139 up\n" : "Network: no NIC found\n");

    int fs_up = ata_init() && fat32_init();
    serial_printf(fs_up ? "Filesystem: FAT32 mounted\n" : "Filesystem: no disk/FAT32 found\n");

    int audio_up = ac97_init();
    serial_printf(audio_up ? "Audio: AC97 ready\n" : "Audio: no codec found\n");

    scheduler_init();
    task_create(bg_task_entry);
    task_create_user(demo_user_task_entry);
    if (net_up) task_create(ping_task_entry);
    pit_set_tick_callback(schedule);
    scheduler_start();
    serial_printf("Scheduler started with %d tasks\n", scheduler_task_count());

    __asm__ volatile ("sti");
    serial_printf("Interrupts enabled\n");

    gui_init();
    serial_printf("Entering GUI main loop\n");
    gui_run();
}
