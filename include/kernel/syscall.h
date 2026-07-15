#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_YIELD     2
#define SYS_GET_TICKS 3 /* returns pit_ticks() (100Hz) in eax */
#define SYS_SLEEP     4 /* ebx = milliseconds; yields until they've passed */
#define SYS_POLL_KEY  5 /* returns -1 if no event pending, else (pressed<<8)|scancode in eax */
#define SYS_BLIT      6 /* ebx = pointer to a caller-owned DOOM_BLIT_W*DOOM_BLIT_H uint32_t 0xRRGGBB buffer */

/* Named IPC channels (kernel/ipc.c) -- the first syscalls to use `ecx`
 * and `edx` (previously-unused-but-architecturally-free general-purpose
 * arg slots in `struct registers`, see include/kernel/idt.h) alongside
 * `ebx`, since ipc_send/ipc_recv each need three values (an id plus a
 * buffer pointer plus a length) where every syscall before this one
 * needed at most one. SYS_IPC_SEND/SYS_IPC_RECV both BLOCK (retry via
 * schedule() from inside the handler, exactly like SYS_SLEEP above)
 * until they can make progress -- see kernel/syscall.c. */
#define SYS_IPC_OPEN  7  /* ebx = pointer to a NUL-terminated channel name; returns channel id (>=0), or -1, in eax */
#define SYS_IPC_SEND  8  /* ebx = channel id, ecx = pointer to buf, edx = len; blocks until queued; returns 0, or -1 for an invalid id/oversized len, in eax */
#define SYS_IPC_RECV  9  /* ebx = channel id, ecx = pointer to buf, edx = cap; blocks until a message arrives; returns the message's actual length (may exceed cap -- truncated, not an error), or -1 for an invalid id, in eax */
#define SYS_IPC_CLOSE 10 /* ebx = channel id; always returns 0 in eax (invalid/already-closed ids are a silent no-op) */

/* Windowed app graphics (gui/compositor.c's gui_app_window_open()/
 * gui_app_window_blit()) -- the gap between SYS_WRITE (text only) and
 * SYS_BLIT (the ENTIRE screen, see DOOM): a normal desktop window a
 * task draws its own pixels into, same as every built-in app (Browser,
 * File Manager, Terminal, ...) already gets, just without their fixed
 * chrome/dock-icon machinery. Backed by a small, separate, fixed-size
 * table (APP_WINDOW_MAX slots -- see gui/compositor.c), NOT the
 * compile-time-fixed windows[]/window_order[] the 8 built-in apps use.
 * A window closes automatically the moment its owning task's state
 * becomes TASK_TERMINATED (checked once per frame, mirroring
 * SYS_BLIT's fs_active/fs_owner_pid cleanup below) -- there is no
 * SYS_WIN_CLOSE in this pass. */
#define SYS_WIN_OPEN  11 /* ebx = pointer to a NUL-terminated title string, ecx = width, edx = height (both capped, see APP_WINDOW_MAX_W/H in gui/compositor.c), esi = style flags bitmask (see WIN_FLAG_* below); returns a window handle (>=0) in eax, or (uint32_t)-1 if w/h are invalid (0, or over the cap) or no free slot exists */
#define SYS_WIN_BLIT  12 /* ebx = window handle from SYS_WIN_OPEN, ecx = pointer to a caller-owned width*height uint32_t 0xAARRGGBB buffer (row-major, top-to-bottom -- top byte is now alpha, 0-255, alpha-composited onto the desktop rather than opaquely copied -- see fb_blend_pixel()/draw_app_windows() in gui/compositor.c), matching the window's own w/h; returns 0 in eax, or (uint32_t)-1 for an invalid/closed handle */

/* SYS_WIN_OPEN style flag bits (the `esi` argument above). Only one bit
 * defined so far: WIN_FLAG_BORDERLESS drops the rounded frame/drop-
 * shadow/title-bar chrome draw_app_windows() (gui/compositor.c) would
 * otherwise draw around the window, so only the app's own alpha-
 * composited pixels ever show -- e.g. a "desktop pet" that wants to
 * float directly on the wallpaper instead of sitting inside a titled
 * box. flags=0 (i.e. omitting this bit) is the original, unchanged
 * bordered/opaque-chrome behavior every existing app-window user still
 * gets. This bit's VALUE must match sdk/zapos.h's ZOS_WIN_BORDERLESS
 * exactly -- there is no shared header across the kernel/SDK boundary,
 * so the two are kept in sync by hand (same pattern the SYS_WIN_OPEN/
 * ZOS_SYS_WIN_OPEN syscall numbers above already use). */
#define WIN_FLAG_BORDERLESS (1u << 0)

/* Fixed resolution SYS_BLIT always copies -- matches the original DOOM's
 * internal resolution. Not user-configurable: the syscall has no way to
 * validate an arbitrary caller-supplied size against what's actually
 * mapped in their address space, so it only ever touches exactly this
 * many bytes (see kernel/syscall.c). */
#define DOOM_BLIT_W 320
#define DOOM_BLIT_H 200

void syscall_init(void);
const char *syscall_last_message(void);
int syscall_message_count(void);

#endif
