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

/* Called from kernel/syscall.c's SYS_WIN_OPEN handler: opens a new
 * app-owned desktop window (a small, fixed-size table of at most
 * APP_WINDOW_MAX slots -- see gui/compositor.c -- entirely separate
 * from the compile-time-fixed windows[]/window_order[] the 8 built-in
 * apps use). `title` is copied into the slot (bounded, truncated if
 * too long); `w`/`h` must both be > 0 and within APP_WINDOW_MAX_W/H, or
 * this fails. Returns a window handle (the slot index, >= 0) on
 * success, or -1 if `w`/`h` are invalid, no free slot exists, or the
 * backing pixel buffer's kmalloc() fails. */
int gui_app_window_open(int owner_pid, const char *title, uint32_t w, uint32_t h);

/* Called from kernel/syscall.c's SYS_WIN_BLIT handler: copies exactly
 * that window's own w*h uint32_t 0xRRGGBB pixels (row-major, top-to-
 * bottom) from `pixels` into its kernel-owned backing buffer, drawn on
 * the next frame by draw_app_windows(). Returns 0 on success, -1 if
 * `handle` is out of range or not currently open. */
int gui_app_window_blit(int handle, const void *pixels);

/* Called from kernel/syscall.c's SYS_WRITE case for every task's
 * output, not just the fullscreen-takeover case above -- a no-op
 * unless `pid` is the one program the Terminal window currently has
 * running in its foreground (see gui/shell.c's "run"/bare-.ELF
 * command), in which case `s` gets appended to that window's own
 * scrollback instead of only the serial log. */
void terminal_route_output(int pid, const char *s);

#endif
