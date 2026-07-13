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

/* Called from kernel/syscall.c's SYS_WRITE case for every task's
 * output, not just the fullscreen-takeover case above -- a no-op
 * unless `pid` is the one program the Terminal window currently has
 * running in its foreground (see gui/shell.c's "run"/bare-.ELF
 * command), in which case `s` gets appended to that window's own
 * scrollback instead of only the serial log. */
void terminal_route_output(int pid, const char *s);

#endif
