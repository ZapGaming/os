#include <drivers/mouse.h>
#include <drivers/ps2.h>
#include <kernel/idt.h>
#include <kernel/io.h>
#include <kernel/serial.h>

#define MOUSE_DATA_PORT 0x60

static uint8_t packet[3];
static int packet_index = 0;

static int mouse_x = 400, mouse_y = 300;
static int bound_w = 1024, bound_h = 768;
static uint8_t buttons = 0;

static void mouse_handler(struct registers *regs) {
    (void)regs;
    uint8_t data = inb(MOUSE_DATA_PORT);

    if (packet_index == 0 && !(data & 0x08)) return; /* resync guard */

    packet[packet_index++] = data;
    if (packet_index < 3) return;
    packet_index = 0;

    buttons = packet[0] & 0x07;

    int dx = packet[1];
    int dy = packet[2];
    if (packet[0] & 0x10) dx -= 256; /* sign extend X */
    if (packet[0] & 0x20) dy -= 256; /* sign extend Y */

    mouse_x += dx;
    mouse_y -= dy; /* PS/2 Y is inverted relative to screen coordinates */

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= bound_w) mouse_x = bound_w - 1;
    if (mouse_y >= bound_h) mouse_y = bound_h - 1;
}

void mouse_set_bounds(int width, int height) {
    bound_w = width;
    bound_h = height;
}

void mouse_get_state(int *x, int *y, uint8_t *btn) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (btn) *btn = buttons;
}

void mouse_init(void) {
    ps2_command(0xD4);
    ps2_write_data(0xF4); /* enable data reporting */
    uint8_t ack = ps2_read_data();
    serial_printf("mouse: enable ack=%x\n", ack);

    register_interrupt_handler(44, mouse_handler); /* IRQ12 */
}
