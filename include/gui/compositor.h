#ifndef GUI_COMPOSITOR_H
#define GUI_COMPOSITOR_H

#include <stdint.h>

void gui_init(void);
void gui_run(void); /* never returns */

/* Called from kernel/syscall.c's SYS_BLIT handler: copies a
 * DOOM_BLIT_W*DOOM_BLIT_H 0xRRGGBB pixel buffer into a kernel-owned
 * staging buffer and puts the GUI into fullscreen-takeover mode (the
 * normal desktop stops drawing; gui_run()'s own loop scales and blits
 * this buffer instead) until `owner_pid` exits. Safe to call from
 * within a syscall handler specifically because it copies synchronously
 * -- see the call site for why the source pointer is only valid right
 * now, not whenever the GUI task's own redraw loop gets around to it. */
void gui_blit_fullscreen(const uint32_t *pixels, int owner_pid);

#endif
