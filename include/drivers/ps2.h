#ifndef DRIVERS_PS2_H
#define DRIVERS_PS2_H

#include <stdint.h>

void ps2_init(void);
void ps2_command(uint8_t cmd);
void ps2_write_data(uint8_t data);
uint8_t ps2_read_data(void);

#endif
