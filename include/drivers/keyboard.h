#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);

/* Returns the next translated ASCII character, or 0 if none is queued. */
char keyboard_getchar(void);

/* Raw scancode access, for keys with no ASCII representation (arrows, etc). */
int keyboard_key_pressed(uint8_t scancode);

#endif
