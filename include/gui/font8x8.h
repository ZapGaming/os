#ifndef GUI_FONT8X8_H
#define GUI_FONT8X8_H

#include <stdint.h>

/* Row i, byte y is a bitmask of the 8 pixels in that row (bit 0 = leftmost). */
extern const uint8_t font8x8_basic[128][8];

#endif
