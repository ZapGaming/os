#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);

/* Returns the next translated ASCII character, or 0 if none is queued. */
char keyboard_getchar(void);

/* Raw scancode access, for keys with no ASCII representation (arrows, etc). */
int keyboard_key_pressed(uint8_t scancode);

/* Raw make/break event queue, independent of the ASCII ring above --
 * feeds SYS_POLL_KEY (see kernel/syscall.c) for ring-3 code (the DOOM
 * port, specifically) that needs press/release edges for keys with no
 * ASCII meaning (arrows, ctrl, alt, shift) rather than translated
 * characters. Returns 1 and fills scancode/pressed if an event was
 * pending, 0 if the queue is empty. */
int keyboard_poll_event(uint8_t *scancode, uint8_t *pressed);

#endif
