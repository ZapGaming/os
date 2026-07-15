/* ZapOS user-mode SDK -- the ONE header a real (host-gcc-built) app needs.
 *
 * Covers the entire syscall surface (numbers 0-12, see
 * include/kernel/syscall.h in the kernel source tree -- these wrappers
 * mirror that file exactly, just with friendlier names and full
 * documentation per function). Every function here is a `static inline`
 * wrapper over a single `int $0x80` -- there is no libc under this: no
 * malloc, no printf, no file I/O beyond what's listed below. That's not
 * an oversight, it's the whole ABI a ZapOS program gets, ring-3, full
 * stop.
 *
 * ZapOS itself has NO trained-model ML runtime anywhere in it. Nothing
 * in this header (or the SDK example apps that use it, including the
 * "AI pet") talks to a neural network of any kind -- see sdk/README.md's
 * honesty section if you came here expecting that.
 *
 * How a syscall works, mechanically: `int $0x80` traps to ring 0 with
 * the syscall number in `eax` and up to three arguments in `ebx`/`ecx`/
 * `edx` (kernel/idt.h's `struct registers` exposes exactly those three
 * general-purpose registers to the handler). A pointer argument (a
 * string, a pixel buffer, an IPC message buffer) is a raw pointer into
 * YOUR OWN address space -- safe to pass directly, because a syscall
 * trap doesn't switch CR3, so the kernel handler runs with your
 * process's own page directory still loaded for as long as the call
 * takes. There is no validation anywhere in this kernel that a pointer
 * you pass is actually mapped -- passing a bad one safely faults only
 * your own task (see kernel/exceptions.c), it can't take down anything
 * else.
 *
 * Build model: compile this alongside your own .c file(s) with plain
 * host gcc, freestanding, targeting the loader's fixed load address --
 * see sdk/README.md section 2 ("full path") for the exact command line
 * and userprogs/user.ld for the linker script every app links against.
 */
#ifndef ZAPOS_SDK_H
#define ZAPOS_SDK_H

/* --- Raw syscall numbers -----------------------------------------------
 * Kept here (not just implicit inside the wrappers below) so anything
 * that needs to double-check ABI numbers against the kernel's own
 * include/kernel/syscall.h can do it at a glance. Keep the two in sync
 * if the kernel ever adds another syscall -- there is no shared build
 * between this SDK and the kernel tree, so nothing enforces that for
 * you. */
#define ZOS_SYS_EXIT      0
#define ZOS_SYS_WRITE     1
#define ZOS_SYS_YIELD     2
#define ZOS_SYS_GET_TICKS 3
#define ZOS_SYS_SLEEP     4
#define ZOS_SYS_POLL_KEY  5
#define ZOS_SYS_BLIT      6
#define ZOS_SYS_IPC_OPEN  7
#define ZOS_SYS_IPC_SEND  8
#define ZOS_SYS_IPC_RECV  9
#define ZOS_SYS_IPC_CLOSE 10
#define ZOS_SYS_WIN_OPEN  11
#define ZOS_SYS_WIN_BLIT  12

/* Fixed resolution zos_blit_fullscreen() always copies -- matches the
 * original DOOM's internal resolution. Not configurable: the syscall
 * has no way to validate an arbitrary caller-supplied size against
 * what's actually mapped in your address space, so it only ever
 * touches exactly this many pixels. */
#define ZOS_BLIT_W 320
#define ZOS_BLIT_H 200

/* Per-window pixel buffer cap for zos_win_open() -- see that function's
 * doc comment. Matches gui/compositor.c's APP_WINDOW_MAX_W/H exactly. */
#define ZOS_WIN_MAX_W 400
#define ZOS_WIN_MAX_H 300

/* -------------------------------------------------------------------- */

/* Writes a NUL-terminated string to the serial log and, if this task
 * happens to be the one program the Terminal window currently has
 * running in its foreground, to that window's own scrollback too.
 * Returns the string's length. This is the only way a ZapOS program
 * can produce human-readable output at all -- there's no libc, so no
 * printf/sprintf either; build any formatting you need by hand (see
 * sdk/examples/aipet/aipet.c's status line for a worked example). */
static inline int zos_write(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_WRITE), "b"(s) : "memory");
    return ret;
}

/* Voluntarily gives up the rest of this task's time slice. Useful in a
 * tight loop that's waiting on something (another task, a tick count)
 * without busy-spinning the CPU pointlessly hard between checks. */
static inline void zos_yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(ZOS_SYS_YIELD) : "memory");
}

/* Never returns. Ends this task immediately; the kernel reclaims its
 * memory (isolated page directory, stack, any app window it still had
 * open -- see zos_win_open()) automatically. There is no exit code --
 * ZapOS's task_exited() doesn't take one, unlike Unix's exit(status). */
static inline void zos_exit(void) {
    __asm__ volatile ("int $0x80" : : "a"(ZOS_SYS_EXIT) : "memory");
    for (;;) {} /* unreachable; satisfies compilers that don't know sys_exit never returns */
}

/* Returns the kernel's PIT tick counter (100 ticks/second, i.e. one
 * tick = 10ms), counting up monotonically since boot. Useful for your
 * own timing/animation logic (see sdk/examples/aipet/aipet.c's decay
 * timer) -- there's no wall-clock/date API in this kernel at all. */
static inline unsigned int zos_get_ticks(void) {
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_GET_TICKS) : "memory");
    return ret;
}

/* Blocks (yielding internally, so other tasks still run) until
 * approximately `ms` milliseconds have passed. Rounded up to the
 * nearest 10ms PIT tick, so e.g. sleep(1) and sleep(10) behave
 * identically. */
static inline void zos_sleep(unsigned int ms) {
    __asm__ volatile ("int $0x80" : : "a"(ZOS_SYS_SLEEP), "b"(ms) : "memory");
}

/* Polls the next pending raw keyboard event (non-blocking). Returns -1
 * if nothing is queued, otherwise `(pressed << 8) | scancode` where
 * `pressed` is 1 for a key-down and 0 for a key-up, and `scancode` is
 * the raw PS/2 Set-1 scancode (NOT ASCII -- e.g. 'f' is 0x21, 'p' is
 * 0x19; see drivers/keyboard.c's scancode_ascii[] table in the kernel
 * source for the full map, or sdk/examples/aipet/aipet.c for a worked
 * example of decoding specific keys). This is the ONLY keyboard input
 * ZapOS gives a program -- there is exactly one global queue shared by
 * every task and the desktop itself; there's no per-window input focus
 * routing (see sdk/README.md's windowed-graphics section). */
static inline int zos_poll_key(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_POLL_KEY) : "memory");
    return ret;
}

/* Takes over the ENTIRE display: copies `pixels` (exactly
 * ZOS_BLIT_W * ZOS_BLIT_H uint32_t 0xRRGGBB values, row-major, top-to-
 * bottom) into a kernel-owned staging buffer and puts the desktop into
 * fullscreen-takeover mode -- the whole windowed desktop (taskbar,
 * every other window) stops drawing entirely and this buffer is scaled
 * up (letterboxed, aspect-preserved) to fill the screen instead, until
 * this task exits. This is what DOOM uses (see userprogs/doom/). For
 * a normal desktop app that wants its own window instead of hijacking
 * the whole screen, use zos_win_open()/zos_win_blit() below instead --
 * that's the gap this SDK exists to close. */
static inline void zos_blit_fullscreen(const unsigned int *pixels) {
    __asm__ volatile ("int $0x80" : : "a"(ZOS_SYS_BLIT), "b"(pixels) : "memory");
}

/* Opens (or joins) a named IPC channel -- the one primitive that lets
 * ANY two tasks exchange data, including two fully isolated ELF-loaded
 * processes that otherwise share nothing at all. `name` is looked up
 * by exact string match (max 15 characters + NUL); a matching already-
 * open channel is joined (refcounted), otherwise a new one is created.
 * Returns a channel id (>= 0) on success, -1 if `name` is NULL, too
 * long, or every channel slot is already in use by a different name.
 * See sdk/README.md's IPC guide for the full rendezvous-by-name model. */
static inline int zos_ipc_open(const char *name) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_IPC_OPEN), "b"(name) : "memory");
    return ret;
}

/* Sends exactly `len` bytes (max 256 -- IPC_MSG_MAX in the kernel
 * source) from `buf` as one message on channel `id`. BLOCKS (yielding
 * internally) until the channel's queue has room for it -- queues hold
 * at most 8 messages per channel. Returns 0 once queued, or -1 for an
 * invalid channel id or an oversized `len`. */
static inline int zos_ipc_send(int id, const void *buf, unsigned int len) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_IPC_SEND), "b"(id), "c"(buf), "d"(len) : "memory");
    return ret;
}

/* Receives the oldest queued message on channel `id` into `buf`
 * (capacity `cap` bytes). BLOCKS until a message arrives. Returns the
 * message's ACTUAL length as originally sent (may exceed `cap` -- only
 * min(actual, cap) bytes are copied, silently truncated, not an error;
 * compare the return value to `cap` if you need to detect that), or -1
 * for an invalid channel id. */
static inline int zos_ipc_recv(int id, void *buf, unsigned int cap) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_IPC_RECV), "b"(id), "c"(buf), "d"(cap) : "memory");
    return ret;
}

/* Decrements channel `id`'s refcount, freeing the slot (and dropping
 * any still-queued messages) once it reaches zero. Always "succeeds"
 * -- an invalid/already-closed id is a silent no-op, never an error. */
static inline void zos_ipc_close(int id) {
    __asm__ volatile ("int $0x80" : : "a"(ZOS_SYS_IPC_CLOSE), "b"(id) : "memory");
}

/* Opens a normal desktop window this task owns -- the gap between
 * zos_write() (text only) and zos_blit_fullscreen() (the ENTIRE
 * screen): a window drawn on the regular windowed desktop alongside
 * the Browser/File Manager/Terminal/etc., with its own rounded chrome
 * and title bar, that YOUR pixels go into.
 *
 * `title` is a NUL-terminated string (copied into the kernel, bounded
 * to 23 characters + NUL -- longer titles are truncated, not
 * rejected). `w`/`h` must both be > 0 and at most ZOS_WIN_MAX_W/H
 * (400x300) -- this is a "one small window per app" feature, not a
 * general-purpose windowing system, and the cap keeps the kernel-side
 * backing buffer's size bounded.
 *
 * Returns a window handle (>= 0) to pass to zos_win_blit() below, or
 * (unsigned)-1 if `w`/`h` are invalid, or if all 4 app-window slots
 * are already in use (ZapOS only allows a handful of these open at
 * once -- see gui/compositor.c's APP_WINDOW_MAX in the kernel source).
 *
 * The window closes itself automatically the moment this task exits --
 * there is no zos_win_close() in this version. There's also no drag
 * support, no close button, and no per-window keyboard focus routing
 * (every task shares the one global zos_poll_key() queue) -- see
 * sdk/README.md's windowed-graphics section for the full, honest list
 * of what this does and doesn't do yet. */
static inline unsigned int zos_win_open(const char *title, unsigned int w, unsigned int h) {
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_WIN_OPEN), "b"(title), "c"(w), "d"(h) : "memory");
    return ret;
}

/* Redraws window `handle` (from zos_win_open()) with the contents of
 * `pixels`: exactly width*height uint32_t 0xRRGGBB values, row-major,
 * top-to-bottom (identical format/orientation to zos_blit_fullscreen()
 * above), where width/height are the exact w/h you passed to
 * zos_win_open() for this handle -- there's no separate size argument
 * here because the window already remembers its own size.
 *
 * There is no double-buffering or vsync of any kind: call this
 * whenever your own state changes and you want the window to reflect
 * it; the compositor redraws every window from its last-blitted buffer
 * on every desktop frame (~60Hz) regardless of whether you've called
 * this recently. Returns 0 on success, or (unsigned)-1 if `handle` is
 * out of range or was never opened (or has already been implicitly
 * closed by... well, nothing but this task exiting, since only this
 * task can hold its own handle). */
static inline unsigned int zos_win_blit(unsigned int handle, const unsigned int *pixels) {
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(ZOS_SYS_WIN_BLIT), "b"(handle), "c"(pixels) : "memory");
    return ret;
}

#endif /* ZAPOS_SDK_H */
