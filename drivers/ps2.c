#include <drivers/ps2.h>
#include <kernel/io.h>
#include <kernel/serial.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02

static void ps2_wait_write(void) {
    for (int timeout = 100000; timeout > 0; timeout--) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL)) return;
    }
}

static void ps2_wait_read(void) {
    for (int timeout = 100000; timeout > 0; timeout--) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) return;
    }
}

void ps2_command(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_COMMAND, cmd);
}

void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}

uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* Initializes the 8042 controller: flush stale output, enable both the
 * keyboard and auxiliary mouse ports, and enable their IRQs in the
 * controller configuration byte. */
void ps2_init(void) {
    /* flush any stale byte */
    while (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) {
        inb(PS2_DATA);
    }

    ps2_command(0xAD); /* disable port 1 */
    ps2_command(0xA7); /* disable port 2 */

    while (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) {
        inb(PS2_DATA);
    }

    ps2_command(0x20); /* read config byte */
    uint8_t config = ps2_read_data();
    config |= 0x03;    /* enable IRQ1 (bit0) and IRQ12 (bit1) */
    config &= ~0x30;   /* ensure both ports' clocks enabled (clear bits 4,5) */
    ps2_command(0x60);
    ps2_write_data(config);

    ps2_command(0xAE); /* enable port 1 (keyboard) */
    ps2_command(0xA8); /* enable port 2 (mouse) */

    serial_printf("ps2: controller initialized\n");
}
