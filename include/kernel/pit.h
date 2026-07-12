#ifndef KERNEL_PIT_H
#define KERNEL_PIT_H

#include <stdint.h>

typedef void (*pit_tick_callback_t)(void);

void pit_init(uint32_t frequency_hz);
uint32_t pit_ticks(void);
void pit_sleep(uint32_t ms);
void pit_set_tick_callback(pit_tick_callback_t cb);

#endif
