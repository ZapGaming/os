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
`ipc_open`, `ipc_send`, `ipc_recv`, `ipc_close`, `win_open`, `win_blit`.
See the syscall reference table below for what each one does — they're
one-to-one with the syscalls `sdk/zapos.h`'s `zos_*` wrappers cover,
just under shorter names and without the `zos_` prefix.

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

Every syscall ZapOS has, as of this SDK (numbers 0–12). "cc builtin"
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
| 11 | `zos_win_open(title,w,h)` | `win_open(title,w,h)` | title string, width, height (≤400×300) | handle ≥0, or -1 | opens this task's own desktop window |
| 12 | `zos_win_blit(handle,px)` | `win_blit(handle,px)` | handle, pixel buffer (w×h 0xRRGGBB) | 0, or -1 | redraws that window with new pixel contents |

Note on #6: `zos_blit_fullscreen()` is deliberately *not* given a `cc`
builtin — it wants a raw pointer to a large fixed-size pixel buffer,
which is far more directly useful to a program that already declares
`int screen[64000];`-style global arrays (an `int` array *is* a
`uint32_t` 0xRRGGBB pixel buffer on this architecture — the same idiom
`win_blit`'s `pixels` argument uses) than a canned wrapper function
would be. Same reasoning applies to `win_blit`'s pixel argument, which
*is* wrapped since the window handle it also needs makes it worth one.

## 3. Windowed graphics guide

This is the capability this SDK exists to add: **a normal desktop
window your app draws its own pixels into**, filling the gap that used
to sit between "print text" (`zos_write`) and "take over the entire
screen" (`zos_blit_fullscreen`, what DOOM does). Every built-in ZapOS
app (Browser, File Manager, Terminal, ...) already draws into a window
like this; `zos_win_open()`/`zos_win_blit()` is the same capability,
exposed to your own code.

- **Pixel format:** `uint32_t`, `0x00RRGGBB` (top byte unused/zero),
  row-major, top-to-bottom — pixel `(x, y)` lives at `buf[y*w + x]`.
  Identical format and orientation to `zos_blit_fullscreen()`.
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
- **No dragging, no close button, no dock/taskbar icon** for app
  windows in this version — real, current limitations, not hidden
  ones. An app window is fixed in position (cascaded by a small offset
  per open window, so a second and third app window don't fully
  overlap) and can only be closed by the app itself exiting.
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

## 5. Getting started in 5 minutes

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
example (open a small window, fill it solid red):

```c
int buf[400]; /* 20x20 -- an int array IS a uint32_t pixel buffer here */

int main() {
    int h;
    int i;
    h = win_open("Quick Win", 20, 20);
    i = 0;
    while (i < 400) {
        buf[i] = 16711680; /* 0xFF0000 */
        i = i + 1;
    }
    win_blit(h, buf);
    while (1) { sleep(1000); } /* keep the task (and window) alive */
}
```

See `sdk/examples/aipet/aipet.c` for the full-path version of this same
idea taken much further — a properly animated face, decaying state,
and keyboard input.

## 6. Honesty / limitations section

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
  are `zos_write` (one-way, to the log/Terminal), the two windowed-
  graphics syscalls, and IPC to another cooperating task. If your app
  needs to persist state across a run, it currently can't, on its own.
- **One global keyboard queue, no window focus.** Covered in section 3
  above — every task and the desktop itself share one event stream.
- **No drag, no close button, no dock icon for app windows.** Covered
  in section 3 above — a real window manager for these is explicit
  follow-on work, not attempted here.
- **The "AI pet" example is not AI in the trained-model sense.**
  Covered at the top of this file and in `sdk/examples/aipet/aipet.c`'s
  own header comment — worth repeating here since it's easy to
  skim past: it's an `if`/`else`-driven counter pair, nothing more.

## Where things live

- `sdk/zapos.h` — the syscall header every full-path app includes.
- `sdk/examples/hello/hello.c` — minimal full-path example.
- `sdk/examples/aipet/aipet.c` — the flagship "Pixel Pet" demo:
  windowed graphics, keyboard input, timer-driven decay, all built on
  this SDK.
- `userprogs/user.ld` — the linker script every full-path app links
  against (shared with this repo's own sample programs — not
  SDK-specific, but required either way).
- `tools/make_disk_image.sh` — the actual mechanism this repo uses to
  get compiled `.ELF` files onto `zapos_disk.img`; follow its existing
  `TEST.ELF`/`DOOM.ELF` pattern for your own app.
