#include <kernel/serial.h>
#include <kernel/io.h>
#include <stdarg.h>
#include <string.h>

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!transmit_empty());
    outb(COM1, c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

static void print_uint(unsigned int val, int base, int uppercase) {
    char buf[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) {
        buf[i++] = digits[val % base];
        val /= base;
    }
    while (i > 0) serial_putc(buf[--i]);
}

void serial_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { serial_putc(*p); continue; }
        p++;
        switch (*p) {
            case 's': serial_write(va_arg(args, const char *)); break;
            case 'd': {
                int v = va_arg(args, int);
                if (v < 0) { serial_putc('-'); v = -v; }
                print_uint((unsigned int)v, 10, 0);
                break;
            }
            case 'u': print_uint(va_arg(args, unsigned int), 10, 0); break;
            case 'x': print_uint(va_arg(args, unsigned int), 16, 0); break;
            case 'c': serial_putc((char)va_arg(args, int)); break;
            case '%': serial_putc('%'); break;
            default: serial_putc('%'); serial_putc(*p); break;
        }
    }
    va_end(args);
}
