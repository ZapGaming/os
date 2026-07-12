#ifndef DRIVERS_MOUSE_H
#define DRIVERS_MOUSE_H

#include <stdint.h>

#define MOUSE_LEFT_BUTTON   0x01
#define MOUSE_RIGHT_BUTTON  0x02
#define MOUSE_MIDDLE_BUTTON 0x04

void mouse_init(void);
void mouse_set_bounds(int width, int height);
void mouse_get_state(int *x, int *y, uint8_t *buttons);

#endif
