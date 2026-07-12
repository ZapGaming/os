#include <kernel/pic.h>
#include <kernel/io.h>

#define PIC1            0x20
#define PIC2            0xA0
#define PIC1_COMMAND    PIC1
#define PIC1_DATA       (PIC1 + 1)
#define PIC2_COMMAND    PIC2
#define PIC2_DATA       (PIC2 + 1)

#define ICW1_ICW4       0x01
#define ICW1_INIT       0x10
#define ICW4_8086       0x01

#define PIC_EOI         0x20

/* Remap the PICs so IRQ0-15 land on IDT vectors 32-47, clear of the
 * CPU exception vectors 0-31 that they overlap with by default. */
void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, 32);      /* master PIC vector offset */
    io_wait();
    outb(PIC2_DATA, 40);      /* slave PIC vector offset */
    io_wait();

    outb(PIC1_DATA, 4);       /* tell master about slave at IRQ2 */
    io_wait();
    outb(PIC2_DATA, 2);       /* tell slave its cascade identity */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(unsigned char irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(unsigned char irq_line) {
    uint16_t port = irq_line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = irq_line < 8 ? irq_line : irq_line - 8;
    uint8_t value = inb(port) | (1 << line);
    outb(port, value);
}

void pic_clear_mask(unsigned char irq_line) {
    uint16_t port = irq_line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t line = irq_line < 8 ? irq_line : irq_line - 8;
    uint8_t value = inb(port) & ~(1 << line);
    outb(port, value);
}
