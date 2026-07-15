# The ZapOS SDK

This is the developer guide for building your own apps for ZapOS — a
from-scratch, freestanding 32-bit x86 OS kernel (custom bootloader,
GUI/compositor, drivers, filesystem, network stack, the works — no
Linux, no BSD, no existing OS code anywhere underneath it). "An app" on
ZapOS means a single, static `ET_EXEC` ELF32 executable, loaded into
its own fully isolated address space (its own page directory — it
cannot see or touch any other task's memory, including the kernel's),
entered at a bare `_start()` function with **no** `argc`/`argv`, **no**
libc, and **no** `main()` wrapper of any kind. Every capability your
app has — printing text, sleeping, reading the keyboard, drawing to
the screen, talking to another process — comes from the syscall
surface this SDK wraps in `sdk/zapos.h`. That's it; there is nothing
else linked in.

**A note on scope, up front, since this SDK's flagship example is
literally named "AI pet":** ZapOS has no machine-learning runtime of
any kind — no tensor math, no trained model, nothing resembling one.
Anywhere you see "AI" in this SDK or its examples, it means a small,
fully deterministic, rule-based state machine (a handful of counters
and `if`/`else` thresholds) — the exact same kind of logic real 1990s
Tamagotchi toys ran on. If you're looking for LLM/neural-net
integration, it isn't here, and nothing in this SDK should be read as
implying otherwise.

## 1. Two ways to build an app

ZapOS gives you two completely different toolchains, with different
tradeoffs. Pick whichever fits what you're doing.

### Quick path: the in-kernel `cc` compiler

ZapOS ships its own tiny, self-hosted C compiler, usable entirely from
inside the OS — no host machine, no cross-compiler, nothing to install.

1. Write your `.c` file using either the Terminal (`cc` is a shell
   command) or the Text Editor built into the File Manager (open any
   `.C`/`.TXT` file and it's editable in place).
2. From the Terminal, compile it straight to a runnable ELF32:
   ```
   cc yourfile.c OUT.ELF
   ```
3. Run it:
   ```
   run OUT.ELF
   ```
   (or double-click `OUT.ELF` in the File Manager).

**The catch:** this compiler only supports a genuinely narrow subset of
C — `int` and pointers only (no `float`, no `char` as its own type, no
`struct`), no preprocessor at all (no `#include`, no macros), and
everything lives in one flat source file. It's real code generation
(a real x86 codegen backend, not an interpreter) producing a real
ELF32 binary, but the language subset is deliberately small. That's
plenty for the kind of stateful, mostly-integer-math app this SDK's
"quick path" pet demo (see section 4 below) needs, but not for
anything that wants real C: reach for the full path below once you
need floats, structs, multiple files, or any libc-shaped convenience.

The builtins this compiler gives you (called like ordinary functions —
see `cc/builtins.c` if you want to see exactly how, they're just
hand-emitted machine code spliced in ahead of your program): `print`,
`print_int`, `yield`, `sleep`, `get_ticks`, `poll_key`, `exit`,
`ipc_open`, `ipc_send`, `ipc_recv`, `ipc_close`, `win_open`, `win_blit`,
`win_move`, `http_request`. See the syscall reference table below for
what each one does — they're one-to-one with the syscalls
`sdk/zapos.h`'s `zos_*` wrappers cover, just under shorter names and
without the `zos_` prefix.

**A caveat specific to `http_request` on this path:** the real syscall
(`SYS_HTTP_REQUEST`) takes a pointer to a `struct zos_http_request` with
14 fields (host, port, method, headers, body, ...) — but this
compiler's language subset has no `struct` syntax at all (see above:
`int`/pointers only). Using `http_request` from a `cc`-compiled program
means hand-laying-out those 14 fields into a plain `int`/pointer array
at the exact byte offsets the real struct uses, matching field order by
hand with no compiler help checking you got it right — possible in
principle (an `int` array is just raw memory here, same idiom
`win_blit`'s pixel buffer already relies on) but genuinely awkward and
easy to get wrong. If you want to make an HTTP request, the full gcc
path below (with `sdk/zapos.h`'s actual `struct zos_http_request`) is
the realistic way to do it.

### Full path: real gcc, off-device

Compile with a completely ordinary host `gcc`, using this SDK's header
and the same freestanding flags the kernel itself is built with — no
special cross-compiler needed, an ordinary Linux `gcc` with 32-bit
multilib support (`gcc-multilib` on Debian/Ubuntu) is all this takes:

```sh
gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
    -fno-builtin -nostdlib -O2 \
    -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
    -c yourapp.c -o yourapp.o
ld -m elf_i386 -T /path/to/zapos/userprogs/user.ld -nostdlib \
    -o YOURAPP.ELF yourapp.o
```

`userprogs/user.ld` links your program at `0x04000000`, the fixed base
address `kernel/elf.c`'s loader reserves for user programs — you don't
need to (and can't) change this. `_start()` is your entry point, called
directly by the loader with no runtime set up first; see
`sdk/examples/hello/hello.c` for the minimal version of this shape.

You get **full C** — whatever your host GCC supports (floats, structs,
multiple translation units, the works) minus libc: no `malloc`, no
`printf`, no `fopen`, none of it. `#include "path/to/sdk/zapos.h"` and
the syscalls it wraps are the entire standard library you have. If you
want heap allocation, string formatting, or anything else libc usually
gives you, you write it yourself (see `sdk/examples/aipet/aipet.c`'s
hand-rolled `append_uint()` for exactly this — there's no `itoa` here
either).

**Getting your `.ELF` onto ZapOS.** There's no way to copy files into
ZapOS from outside at runtime — the disk image is built once, before
boot. This repo's own build already does exactly this for its sample
programs (`TEST.ELF`, `EVIL.ELF`, `DOOM.ELF`, ...): `tools/make_disk_image.sh`
formats a 64MB FAT32 image with `mformat`/`mcopy` (the `mtools`
package — no root, no loop devices needed) and copies each compiled
`.ELF` onto it directly. Follow that exact pattern for your own app:
add a few lines to that script (or run the equivalent `mcopy -i
zapos_disk.img yourapp.ELF ::/YOURAPP.ELF` by hand against an existing
image), then `make iso disk` to rebuild `zapos_disk.img`/`zapos.iso`
with your program included. Once it's on the image, launch it from the
File Manager (double-click) or the Terminal (`run YOURAPP.ELF`) exactly
like any of the built-in sample programs.

## 2. Full syscall reference

Every syscall ZapOS has, as of this SDK (numbers 0–14). "cc builtin"
is the in-kernel compiler's name for the same operation where one
exists — both call the exact same underlying syscall, they're just two
different source-level spellings of it.

| # | `sdk/zapos.h` | cc builtin | args | returns | what it does |
|---|---|---|---|---|---|
| 0 | `zos_exit()` | `exit(code)` | (`code` accepted, ignored) | never returns | ends this task immediately; the kernel reclaims its memory (including any open app window) |
| 1 | `zos_write(s)` | `print(s)` | `const char *s` (NUL-terminated) | length written | writes to the serial log, and to the Terminal's scrollback if this task is its current foreground program |
| 2 | `zos_yield()` | `yield()` | — | 0 | gives up the rest of this task's time slice |
| 3 | `zos_get_ticks()` | `get_ticks()` | — | tick count | PIT ticks since boot, 100/sec (1 tick = 10ms) |
| 4 | `zos_sleep(ms)` | `sleep(ms)` | `unsigned ms` | 0 | blocks (yielding internally) for approximately `ms` milliseconds, rounded up to the nearest 10ms tick |
| 5 | `zos_poll_key()` | `poll_key()` | — | `-1` or `(pressed<<8)\|scancode` | next pending raw keyboard event, non-blocking (see section 4's input notes) |
| 6 | `zos_blit_fullscreen(px)` | *(not wrapped — see note)* | `const uint32_t *px` (320×200 0xRRGGBB) | — | takes over the ENTIRE screen with this pixel buffer until the task exits (what DOOM uses) |
| 7 | `zos_ipc_open(name)` | `ipc_open(name)` | `const char *name` (≤15 chars) | channel id ≥0, or -1 | opens/joins a named IPC channel |
| 8 | `zos_ipc_send(id,buf,len)` | `ipc_send(id,buf,len)` | id, buffer, length (≤256) | 0, or -1 | blocking send of one message |
| 9 | `zos_ipc_recv(id,buf,cap)` | `ipc_recv(id,buf,cap)` | id, buffer, capacity | actual message length, or -1 | blocking receive (FIFO), truncates to `cap` if the message is bigger |
| 10 | `zos_ipc_close(id)` | `ipc_close(id)` | id | 0 (always) | leaves a channel; frees it once every opener has left |
| 11 | `zos_win_open(title,w,h,flags)` | `win_open(title,w,h,flags)` | title string, width, height (≤400×300), style flags (`ZOS_WIN_BORDERLESS` bit, or 0) | handle ≥0, or -1 | opens this task's own desktop window, bordered or (with the flag) a chrome-less floating one |
| 12 | `zos_win_blit(handle,px)` | `win_blit(handle,px)` | handle, pixel buffer (w×h 0xAARRGGBB, top byte = alpha) | 0, or -1 | redraws that window with new pixel contents, alpha-composited onto the desktop |
| 13 | `zos_win_move(handle,x,y)` | `win_move(handle,x,y)` | handle, new x, new y (signed) | 0, or -1 | repositions that window — no screen-bounds clamping, moving off-screen just clips/hides it |
| 14 | `zos_http_request(req)` | `http_request(req)` | pointer to a `struct zos_http_request` (see section 5) | 0, or -1 | makes a blocking outbound HTTP/HTTPS request (GET/POST/any method, custom headers/body) |

Note on #6: `zos_blit_fullscreen()` is deliberately *not* given a `cc`
builtin — it wants a raw pointer to a large fixed-size pixel buffer,
which is far more directly useful to a program that already declares
`int screen[64000];`-style global arrays (an `int` array *is* a
`uint32_t` 0xRRGGBB pixel buffer on this architecture — the same idiom
`win_blit`'s `pixels` argument uses) than a canned wrapper function
would be. Same reasoning applies to `win_blit`'s pixel argument, which
*is* wrapped since the window handle it also needs makes it worth one.

Note on #14: `http_request`'s `cc` builtin *is* wired up (every syscall
gets one, for consistency), but see section 1's caveat above — this
compiler has no `struct` syntax, so using it means hand-laying-out
`struct zos_http_request`'s 14 fields into a raw `int`/pointer array by
hand. The full gcc path (section 5 below) is the realistic way to
actually make an HTTP request.

## 3. Windowed graphics guide

This is the capability this SDK exists to add: **a normal desktop
window your app draws its own pixels into**, filling the gap that used
to sit between "print text" (`zos_write`) and "take over the entire
screen" (`zos_blit_fullscreen`, what DOOM does). Every built-in ZapOS
app (Browser, File Manager, Terminal, ...) already draws into a window
like this; `zos_win_open()`/`zos_win_blit()` is the same capability,
exposed to your own code.

- **Pixel format:** `uint32_t`, `0xAARRGGBB` — top byte is now a real
  alpha channel (0-255), not unused padding — row-major, top-to-bottom,
  pixel `(x, y)` lives at `buf[y*w + x]`. The compositor alpha-composites
  every app-window pixel onto whatever's behind it (`fb_blend_pixel()`)
  rather than opaquely copying it: alpha=0 is fully transparent (the
  desktop/whatever's underneath shows through untouched), alpha=255 is
  fully opaque, values in between blend proportionally. This applies to
  *every* app window, bordered or borderless — one consistent format,
  not two. If you don't need transparency, just set alpha=255 on every
  pixel (`0xFF000000 | your_0xRRGGBB_color`) and it behaves exactly like
  a plain opaque blit. **This is different from `zos_blit_fullscreen()`**,
  which is unrelated and still `0x00RRGGBB`/opaque-only — only app
  windows gained an alpha channel.
- **Style flags — bordered or borderless:** `zos_win_open()`'s 4th
  argument is a flags bitmask. `0` gives you the original window: a
  rounded frame, a drop shadow, and a title bar with your `title` text
  drawn in it. `ZOS_WIN_BORDERLESS` instead draws NONE of that chrome —
  no frame, no shadow, no title bar — just your own alpha-composited
  pixels floating directly on the desktop at the same cascaded slot
  position a bordered window would use, with no inset/offset around it.
  `title` is still stored (and still shows up if you later imagine a
  taskbar entry), but a borderless window never draws it anywhere, since
  there's no title bar for it to appear in. This is what
  `sdk/examples/aipet/aipet.c` uses to be a real floating desktop pet
  instead of a pet-shaped box.
- **Size cap:** 400×300 (`ZOS_WIN_MAX_W`/`ZOS_WIN_MAX_H` in
  `sdk/zapos.h`) — this is "room for a handful of small app windows,"
  not a general-purpose windowing system. At most 4 app windows total
  can be open across the whole system at once (a small fixed table in
  the kernel, `gui/compositor.c`'s `APP_WINDOW_MAX`) — a 5th
  `zos_win_open()` call while 4 are already open fails (returns
  `(unsigned)-1`) until one of the existing 4 closes.
- **No double-buffering or vsync.** Call `zos_win_blit()` whenever your
  own state changes and you want it reflected — there's no "frame" to
  synchronize with on your end. The compositor redraws every open
  window from its last-blitted buffer on every desktop frame (roughly
  60Hz) regardless of how often you've actually called `zos_win_blit()`
  — call it too rarely and your window just shows stale pixels, call it
  every loop iteration (as the pet example does) and that's fine too.
- **Windows close automatically when your task exits** — there is no
  `zos_win_close()` in this version. This mirrors exactly how
  `zos_blit_fullscreen()`'s fullscreen takeover already works: the
  kernel notices your task's state is `TASK_TERMINATED` and reclaims
  the window on its own, once per desktop frame.
- **No dragging (by the user), no close button, no dock/taskbar icon**
  for app windows in this version — real, current limitations, not
  hidden ones. A window still can't be closed except by the app itself
  exiting, and the desktop user can't click-and-drag it around like the
  8 built-in windows.
- **The app itself CAN reposition its own window at runtime**, via
  `zos_win_move(handle, x, y)` (`win_move` on the `cc` path) — the
  piece that turns a fixed-cascaded-position window into something that
  can actually move, e.g. a desktop pet that wanders instead of sitting
  still (see `sdk/examples/aipet/aipet.c`). A window opens at the same
  cascaded-by-slot default position as before (`40 + slot*30` for both
  x and y) and stays there until/unless the app calls `zos_win_move()`.
  There is **no screen-bounds clamping** — moving your window off-screen
  (fully or partially) just renders it clipped/invisible there, exactly
  like any other out-of-bounds pixel write already safely does nothing;
  staying on-screen, if you care, is entirely your own responsibility.
  There's also no "query screen resolution" syscall, so if you need to
  bounce within a wander box, pick a conservative fixed one well inside
  the smallest resolution this kernel supports (1024×768) rather than
  guessing the real screen size — see aipet.c's own wander box for a
  worked example.
- **No per-window keyboard focus.** There is exactly **one** global
  keyboard event queue (`zos_poll_key()`), shared by the desktop itself
  and every task currently running — there's no concept of "this
  window is focused, so only it gets keystrokes." If more than one app
  is polling for keys at the same time, both see the same events. For
  a single app window running at a time (the common case for a
  demo/example app), this doesn't matter in practice; be aware of it if
  you're running several key-driven programs at once.

## 4. IPC guide

`zos_ipc_open`/`send`/`recv`/`close` give any two ZapOS tasks a way to
exchange data even when they share absolutely nothing else — including
two separately-launched, fully isolated ELF programs that don't know
each other's pid and have entirely private address spaces.

- **Rendezvous by name, not by handle.** You don't need a pid or a file
  descriptor inherited from a common parent (there's no `fork()` on
  ZapOS at all) — both sides just call `zos_ipc_open("some-name")` with
  the same string, the same way two Unix processes might meet on a
  named FIFO path instead of an inherited fd. Whichever side calls it
  first creates the channel; the second joins the same one
  (refcounted) and gets back the same channel id. Names are short —
  15 characters plus the NUL, no more.
- **Blocking semantics.** `zos_ipc_send`/`zos_ipc_recv` both block (by
  internally yielding the CPU in a loop, not by spinning hard) until
  they can make progress: `send` waits for queue space, `recv` waits
  for a message to exist. There is no non-blocking/"try" variant
  exposed at the syscall level — if you need a timeout, poll
  `zos_get_ticks()` yourself around your own retry logic instead.
- **Size and depth caps.** Each message is at most 256 bytes; each
  channel holds at most 8 queued messages before a sender blocks
  waiting for room. A `recv` into a smaller buffer than the message
  that arrives silently truncates (copies only what fits) — compare
  the return value (the message's real length) against your buffer's
  capacity if you need to detect that happened.
- **Cleanup is refcounted, not "last one out clears it."** Every
  `zos_ipc_open()` on a given name increments a refcount;
  `zos_ipc_close()` decrements it; the channel (and anything still
  queued on it) is only actually freed once that refcount hits zero.
  You don't need to coordinate who "owns" cleanup — just close your own
  end when you're done with it.

## 5. Networking guide

`zos_http_request()` (`SYS_HTTP_REQUEST`, #14) is what makes a ZapOS
app able to talk to the outside world beyond the built-in Browser: a
generic outbound HTTP or HTTPS request — GET, POST, or any other
method, with whatever headers and body you choose.

**Read this before assuming otherwise:** ZapOS has **no bundled AI
integration** anywhere in it, ships **no API key**, and this syscall
does not itself talk to any AI provider (or any other specific web
service) on its own. It is generic HTTP client capability, full stop —
exactly what's needed to call a real AI provider's chat/completions
API, a weather API, or literally any other web API that needs POST +
custom headers, but **you** supply your own endpoint (host/path) and
your own credentials (typically an `Authorization: ...` header). This
function has no idea what service it's talking to.

- **What it can do:** plain HTTP or TLS 1.2 HTTPS (`use_tls`), any
  request method (`method`, e.g. `"GET"`/`"POST"`), a block of custom
  headers you format yourself (`extra_headers`, e.g.
  `"Authorization: Bearer sk-...\r\nContent-Type: application/json\r\n"`),
  and an optional raw request body (`body`/`body_len`, e.g. a JSON
  payload for a POST). It transparently follows redirects **for GET
  only** (never for POST — resubmitting a POST body to a redirect
  target isn't correct HTTP behavior), sends/stores cookies, and decodes
  chunked transfer-encoding and gzip/deflate content-encoding, same as
  the kernel's own Browser gets from the underlying `net/http.c`.
- **The struct-in-memory ABI.** This syscall needs far more input/
  output fields than the 5 available registers can hold, so `ebx` is a
  pointer to a caller-owned `struct zos_http_request` (defined in
  `sdk/zapos.h`, mirrored byte-for-byte in the kernel's own
  `include/kernel/syscall.h` — the two copies are kept in sync by hand,
  same pattern every other syscall constant in this SDK already uses).
  You fill in the input fields (`host`, `port`, `use_tls`, `method`,
  `path`, `extra_headers`, `body`, `body_len`, `response_buf`,
  `response_cap`, `content_type_buf`, `content_type_cap`) before the
  call; the kernel writes `status_out` and `response_len_out` back
  through the same pointer, which you read after the call returns.
- **It blocks.** Real network I/O (DNS, TCP or TLS handshake, waiting
  for bytes) takes real wall-clock time — this call does not return
  until the request finishes or fails, exactly like
  `zos_sleep()`/`zos_ipc_send()`/`zos_ipc_recv()` already block by
  yielding internally rather than spinning.
- **Return value vs. HTTP status — don't confuse the two.** The
  syscall itself returns 0 if the request mechanically completed (DNS
  resolved, TCP/TLS connected, a response came back) — check
  `req.status_out` for the actual HTTP status code, which might well be
  a 404 or 500 (that's still a return value of 0 here; the SERVER
  responded with an error, the request itself didn't fail). It returns
  `(unsigned)-1` only if DNS resolution, the TCP connection, or the TLS
  handshake itself failed — in that case `status_out` is 0.

A minimal worked example (a plain GET, no headers, no body) —
see `sdk/examples/http_fetch/http_fetch.c` for the complete, runnable
version of this:

```c
struct zos_http_request req;
req.host = "example.com";
req.port = 0;            /* 0 = default (80, since use_tls is 0) */
req.use_tls = 0;
req.method = "GET";
req.path = "/";
req.extra_headers = 0;   /* NULL -- no custom headers */
req.body = 0;            /* NULL -- no request body */
req.body_len = 0;
req.response_buf = response_buf;       /* your own buffer */
req.response_cap = sizeof(response_buf) - 1;
req.content_type_buf = content_type_buf; /* or NULL to skip */
req.content_type_cap = sizeof(content_type_buf);

unsigned int r = zos_http_request(&req);
if (r == (unsigned int)-1) {
    /* DNS/TCP/TLS failed -- no response at all */
} else {
    /* req.status_out is the HTTP status; response_buf/response_len_out
       hold the decoded body */
}
```

Adapting this into a POST against a real AI provider's API is a matter
of setting `method = "POST"`, `use_tls = 1`, pointing `extra_headers` at
a string containing your own `Authorization`/`Content-Type` lines, and
`body`/`body_len` at your own hand-built JSON payload — see the large
comment block at the bottom of `sdk/examples/http_fetch/http_fetch.c`
for the exact shape of this (marked clearly as illustrative, since this
repo has no real API key to test it against and does not pretend
otherwise).

## 6. Getting started in 5 minutes

**Quick path** (paste this into a file via the Text Editor, or type it
directly into the Terminal with whatever ZapOS gives you for creating
one — see the Terminal's own `help` for exact file-creation commands):

```c
/* PET.C -- five-minute quick-path demo */
int main() {
    int t;
    t = 0;
    while (t < 10) {
        print("tick ");
        print_int(t);
        print("\n");
        sleep(500);
        t = t + 1;
    }
    return 0;
}
```

Then, from the Terminal:

```
cc PET.C PET.ELF
run PET.ELF
```

**Full path**, using this SDK's own hello-world example:

```sh
cd sdk/examples/hello
gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
    -fno-builtin -nostdlib -O2 -mno-sse -mno-sse2 -mno-mmx \
    -mno-80387 -mgeneral-regs-only -c hello.c -o hello.o
ld -m elf_i386 -T ../../../userprogs/user.ld -nostdlib -o HELLO.ELF hello.o
```

Then get `HELLO.ELF` onto `zapos_disk.img` (section 1 above) and
`run HELLO.ELF` from the Terminal, or double-click it in the File
Manager.

**Windowed graphics from the quick path, too** — `win_open`/`win_blit`
are ordinary `cc` builtins, no special syntax needed. A minimal
example (open a small window, fill it solid, fully-opaque red — note
the top byte of every pixel is now alpha, so it has to be `0xFF`, not
`0x00`, or the window renders fully transparent/invisible):

```c
int buf[400]; /* 20x20 -- an int array IS a uint32_t pixel buffer here */

int main() {
    int h;
    int i;
    h = win_open("Quick Win", 20, 20, 0); /* 0 = bordered (the default style) */
    i = 0;
    while (i < 400) {
        buf[i] = -65536; /* 0xFFFF0000 as a 32-bit signed int: alpha=0xFF, red=0xFF, green=blue=0x00 */
        i = i + 1;
    }
    win_blit(h, buf);
    while (1) { sleep(1000); } /* keep the task (and window) alive */
}
```

See `sdk/examples/aipet/aipet.c` for the full-path version of this same
idea taken much further — a properly animated face, decaying state,
keyboard input, and (unlike the minimal example above) a real
borderless, transparent floating desktop pet: no rectangular window box
at all, just the face's own pixels composited straight onto the
desktop.

## 7. Honesty / limitations section

Read this before you build something that assumes otherwise:

- **No libc, on either path.** No `malloc`/`free`, no `printf`/`sprintf`,
  no `memcpy`/`strlen` unless you write them yourself (or bring your
  own — nothing stops you from vendoring a tiny freestanding libc into
  your own app's sources on the full path). The quick (`cc`) path
  additionally has no way to `#include` anything at all, ever.
- **No floats in the quick (`cc`) path.** `int` and pointers only — not
  a temporary limitation, a fundamental one of that compiler's
  supported grammar. Full floating point is available on the full gcc
  path (ordinary `float`/`double`), with one kernel-wide caveat: this
  kernel is built `-mgeneral-regs-only -mno-sse -mno-80387` for most
  code, meaning float math needs the plain x87 FPU path specifically
  (see `userprogs/fputest.c` and the main README's FPU section for the
  exact flags that get you there) — SSE floating point is not
  available anywhere in this kernel.
- **No filesystem access from your own app, at all.** There is
  currently no "open/read/write a file" syscall exposed to user
  programs — the File Manager and Terminal read/write files, but they
  do it from kernel-side code, not by exposing a syscall your ELF
  binary can call. Your app's only ways to communicate with the world
  are `zos_write` (one-way, to the log/Terminal), the windowed-graphics
  syscalls, `zos_http_request()` (outbound network only — there is no
  way to LISTEN/accept an inbound connection), and IPC to another
  cooperating task. If your app needs to persist state across a run, it
  currently can't, on its own.
- **One global keyboard queue, no window focus.** Covered in section 3
  above — every task and the desktop itself share one event stream.
- **No drag (by the user), no close button, no dock icon for app
  windows.** Covered in section 3 above — an app CAN reposition its own
  window at runtime (`zos_win_move()`), but the user still can't
  click-and-drag one, and there's no close button or taskbar entry; a
  real window manager for these is explicit follow-on work, not
  attempted here.
- **The "AI pet" example is not AI in the trained-model sense.**
  Covered at the top of this file and in `sdk/examples/aipet/aipet.c`'s
  own header comment — worth repeating here since it's easy to
  skim past: it's an `if`/`else`-driven counter pair, nothing more. It
  now wanders around the desktop (via `zos_win_move()`) instead of
  sitting in one fixed spot, but that's still just position bookkeeping
  in a loop — no smarter than before.
- **`zos_http_request()` is generic HTTP capability, not bundled AI
  access.** Covered in section 5 above — ZapOS ships no API key and
  this syscall doesn't talk to any particular service on its own; it's
  a bring-your-own-endpoint-and-credentials HTTP client, the same way a
  real OS's socket API doesn't know or care what you connect it to.

## Where things live

- `sdk/zapos.h` — the syscall header every full-path app includes.
- `sdk/examples/hello/hello.c` — minimal full-path example.
- `sdk/examples/aipet/aipet.c` — the flagship "Pixel Pet" demo: a real
  borderless, transparent floating desktop pet (`ZOS_WIN_BORDERLESS`,
  alpha-composited pixels, no rectangular window box), with keyboard
  input, timer-driven decay, and now (via `zos_win_move()`) a wandering
  bounce-around-the-desktop movement pattern, all built on this SDK.
- `sdk/examples/http_fetch/http_fetch.c` — demonstrates
  `zos_http_request()`: a plain GET to example.com, printing the status
  code and a snippet of the response, plus a large illustrative (not
  executable) comment showing how to adapt the same call into a POST
  against a real AI provider's API with your own key.
- `userprogs/user.ld` — the linker script every full-path app links
  against (shared with this repo's own sample programs — not
  SDK-specific, but required either way).
- `tools/make_disk_image.sh` — the actual mechanism this repo uses to
  get compiled `.ELF` files onto `zapos_disk.img`; follow its existing
  `TEST.ELF`/`DOOM.ELF` pattern for your own app.
