#include <kernel/pit.h>
#include <kernel/idt.h>
#include <kernel/io.h>

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQ 1193182

static volatile uint32_t ticks = 0;
static uint32_t tick_rate_hz = 100;

static void pit_handler(struct registers *regs) {
    (void)regs;
    ticks++;
}

void pit_init(uint32_t frequency_hz) {
    tick_rate_hz = frequency_hz;
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    register_interrupt_handler(32, pit_handler); /* IRQ0 */
}

uint32_t pit_ticks(void) {
    return ticks;
}

void pit_sleep(uint32_t ms) {
    uint32_t target = ticks + (ms * tick_rate_hz) / 1000;
    while (ticks < target) {
        __asm__ volatile ("hlt");
    }
}
