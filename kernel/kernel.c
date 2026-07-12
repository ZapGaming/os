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
#include <stdint.h>

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    serial_init();
    serial_printf("ZapOS booting...\n");
    serial_printf("multiboot magic=%x info=%x\n", magic, mb_info_addr);

    gdt_init();
    serial_printf("GDT loaded\n");

    idt_init();
    serial_printf("IDT loaded\n");

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

    __asm__ volatile ("sti");
    serial_printf("Interrupts enabled\n");

    gui_init();
    serial_printf("Entering GUI main loop\n");
    gui_run();
}
