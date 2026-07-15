#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <stdint.h>

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

/* SYS_WIN_MOVE (gui/compositor.c's gui_app_window_move()) -- repositions
 * an already-open app window. `ebx` = window handle from SYS_WIN_OPEN,
 * `ecx`/`edx` = new x/y (both cast to `int` on the kernel side -- an app
 * might legitimately want to move slightly negative mid-bounce, before
 * clamping itself back on the next step). There is NO screen-bounds
 * clamping here: a window moved off-screen just renders clipped/
 * invisible, exactly like any other out-of-bounds fb_put_pixel() call
 * already safely no-ops -- staying on-screen is the app's own
 * responsibility, not a kernel-enforced constraint. Returns 0 in eax on
 * success, or (uint32_t)-1 for an invalid/closed handle. */
#define SYS_WIN_MOVE  13 /* ebx = window handle from SYS_WIN_OPEN, ecx = new x (signed), edx = new y (signed); returns 0, or (uint32_t)-1 for an invalid handle */

/* SYS_HTTP_REQUEST (net/http.c's http_request()) -- a generic outbound
 * HTTP(S) request (GET, POST, or any other method, with caller-supplied
 * headers and body) exposed to ring-3. This is what gives a ZapOS app
 * the ability to call a real web API (a weather service, an AI
 * provider's chat-completions endpoint, anything) that needs POST +
 * custom headers (e.g. Authorization) rather than a bare GET -- ZapOS
 * itself ships NO API keys and talks to NO AI service anywhere in the
 * kernel; this syscall is generic HTTP client capability, full stop.
 * The caller supplies their own endpoint/credentials in `extra_headers`/
 * `body`.
 *
 * Too many parameters for registers (host, port, tls flag, method,
 * path, headers, body, body_len, response buffer + cap, status/len-out,
 * content-type buffer + cap -- 13+ fields), so this uses the standard
 * "caller-owned struct in memory" ABI instead of packing more registers:
 * `ebx` = a pointer to a `struct zos_http_request` (defined below) that
 * lives in the CALLING task's own address space. Safe to dereference
 * directly for the same reason every other pointer-argument syscall
 * here is: a syscall trap doesn't change CR3, so the caller's own page
 * directory is still loaded for the whole duration of this handler --
 * and since CR3 doesn't change, the struct's own pointer FIELDS (host/
 * method/path/extra_headers/body/response_buf/content_type_buf) are
 * ALSO safe to dereference directly, for the identical reason. This is
 * a BLOCKING syscall (real network I/O) -- exactly like SYS_IPC_SEND/
 * SYS_IPC_RECV/SYS_SLEEP already block by yielding internally; nothing
 * about that changes here, http_request()'s own receive loop already
 * yields (pit_sleep()) while waiting for more data.
 *
 * Returns 0 in eax if the request mechanically completed (even for a
 * non-2xx HTTP status -- check req->status_out for that), or
 * (uint32_t)-1 if DNS/TCP/TLS itself failed. `req->status_out` and
 * `req->response_len_out` are written directly back through the same
 * already-dereferenced pointer, visible to the caller the instant the
 * syscall returns. */
#define SYS_HTTP_REQUEST 14 /* ebx = pointer to a caller-owned struct zos_http_request (see below); returns 0 in eax on mechanical success (check status_out for the HTTP status), or (uint32_t)-1 if DNS/TCP/TLS failed */

/* The struct SYS_HTTP_REQUEST's `ebx` points to. Field order/types must
 * stay EXACTLY in sync with sdk/zapos.h's hand-duplicated copy -- there
 * is no shared header across the kernel/SDK boundary (same pattern
 * every other syscall here already uses for its constants), but since
 * both sides are compiled by the same 32-bit x86 C ABI, identical field
 * order/types guarantees an identical memory layout.
 *
 * Fields above the "written by the kernel" line are read-only inputs
 * from the caller; fields below it are outputs the kernel writes back
 * through this same pointer before returning. `host`/`method`/`path` are
 * bounds-copied into small fixed kernel-side stack buffers before use
 * (see kernel/syscall.c's SYS_HTTP_REQUEST case) -- a non-NUL-terminated
 * or absurdly long string just gets truncated, not a crash. */
struct zos_http_request {
    const char *host;            /* NUL-terminated hostname */
    uint32_t port;                /* 0 = default (80 or 443 depending on use_tls) */
    uint32_t use_tls;             /* 0 or 1 */
    const char *method;           /* NUL-terminated, e.g. "GET" or "POST" */
    const char *path;             /* NUL-terminated request path, e.g. "/v1/messages" */
    const char *extra_headers;    /* NULL, or NUL-terminated "Name: value\r\n..." block */
    const void *body;             /* NULL, or a request body buffer */
    uint32_t body_len;             /* 0 if body is NULL */
    char *response_buf;           /* caller-owned output buffer for the response body */
    uint32_t response_cap;        /* capacity of response_buf */
    char *content_type_buf;       /* NULL to skip, else caller-owned output buffer */
    uint32_t content_type_cap;    /* capacity of content_type_buf */
    /* --- written by the kernel, read by the caller after the syscall returns --- */
    int status_out;               /* HTTP status code, or 0 if the request itself failed (DNS/TCP/TLS) */
    uint32_t response_len_out;    /* actual response body length copied into response_buf */
};

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
