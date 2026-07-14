#include <kernel/power.h>
#include <kernel/serial.h>
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void power_reboot(void) {
    serial_printf("power: reboot requested\n");
    __asm__ volatile ("cli");

    /* Pulse the PS/2 controller reset line. This works on QEMU and remains
     * the most broadly compatible fallback on legacy x86 hardware. */
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if ((inb(0x64) & 0x02) == 0) break;
    }
    outb(0x64, 0xFE);

    /* If the controller ignored us, force a triple fault. */
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) empty_idt = { 0, 0 };
    __asm__ volatile ("lidt %0\nint $3" : : "m"(empty_idt));

    for (;;) __asm__ volatile ("hlt");
}

void power_shutdown(void) {
    serial_printf("power: shutdown requested\n");
    __asm__ volatile ("cli");

    /* Emulator poweroff ports. QEMU/Bochs accept 0x604, older Bochs accepts
     * 0xB004, and VirtualBox commonly accepts 0x4004. Real machines simply
     * ignore unknown I/O ports, so trying all three is harmless. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    for (;;) __asm__ volatile ("hlt");
}
