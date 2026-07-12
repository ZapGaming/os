# ZapOS

A from-scratch 32-bit x86 operating system: custom bootloader integration,
kernel, memory manager, drivers, and GUI — no Linux kernel, no libc, no
existing OS underneath it. It boots as a bootable `.iso` in QEMU/VirtualBox.

This is a first milestone, not a finished "everyday OS." Realistically,
matching a modern desktop OS (real filesystem, full TCP/IP + wifi, GPU
drivers, audio, app ecosystem) is a multi-year effort even for full teams
(see SerenityOS, Haiku, ReactOS). What's here is a solid, working foundation
you can keep building on.

## What's implemented

- **Boot**: GRUB (Multiboot2) loads the kernel and hands it a linear
  graphics framebuffer — no real-mode VBE/VGA BIOS calls needed.
- **Core kernel**: GDT, IDT, ISR/IRQ dispatch, 8259 PIC remapping, PIT timer.
- **Memory**: physical frame bitmap allocator, paging (4 GiB identity-mapped
  via 4 MiB PSE pages), a first-fit `kmalloc`/`kfree` kernel heap.
- **Drivers**: PS/2 keyboard (scancode set 1 → ASCII, shift/caps/ctrl
  state) and PS/2 mouse (relative packet decoding, button state).
- **Graphics**: linear framebuffer primitives (pixels, rects, lines, filled
  triangles, alpha blending, gradients) with an embedded 8x8 bitmap font.
- **GUI**: a compositor with a themed desktop background, draggable
  windows with title bars/close buttons/drop shadows, a taskbar with a
  live clock, and a custom-drawn cursor. Double-buffered to avoid tearing.
- **Serial debug console** (COM1) for early boot logging — see it with
  `make run` or `-serial stdio`.

## What's stubbed / not yet built

This session prioritized "bootable kernel with a real GUI" as the first
milestone. Not yet implemented:

- **Networking**: no NIC driver, no TCP/IP stack yet.
- **Audio**: no sound driver yet.
- **Filesystem**: no on-disk filesystem or persistent storage driver (ATA/AHCI).
- **Multitasking**: no process/thread scheduler yet — everything runs in
  the kernel's single execution context.
- **User mode**: no ring-3 execution, no syscall interface yet.

See "Roadmap" below for how each of these would actually get built.

## Building and running

Requires: `gcc` (with 32-bit multilib support), `nasm`, `grub-mkrescue`,
`xorriso`, `qemu-system-x86` (all installed via apt in this environment).

```sh
make          # compile the kernel (build/kernel.elf)
make iso      # package it as zapos.iso via GRUB
make run      # build the ISO and boot it in QEMU with serial output on stdio
```

To run the ISO in VirtualBox/VMware/real hardware instead: just point the
VM's CD/DVD drive at `zapos.iso`, boot mode = legacy BIOS (not UEFI/Secure
Boot yet — that would need a `grub-mkrescue --efi` build and a different
Multiboot path).

## Architecture / directory layout

```
boot/            multiboot2 header + real assembly entry point
kernel/          GDT/IDT/ISR/IRQ, PIC, PIT, paging, physical memory
                 manager, kernel heap, multiboot info parser, serial console
drivers/         PS/2 controller, keyboard, mouse (ps2.c/keyboard.c/mouse.c)
gui/             framebuffer primitives, bitmap font, window compositor
include/         public headers, mirroring kernel/ and drivers/ and gui/
linker.ld        places the kernel at 1 MiB physical/virtual (identity-mapped)
Makefile         freestanding i386 build (gcc -m32 -ffreestanding -nostdlib)
iso/grub.cfg     GRUB menu entry (multiboot2 /boot/kernel.elf)
```

Everything runs in ring 0, identity-mapped, single-threaded, cooperative
(the GUI's `gui_run()` loop polls the mouse/keyboard and redraws ~60 times
a second via a PIT-timed sleep).

## Third-party assets

- `gui/font8x8.c` — 8x8 bitmap font glyphs, public domain (CC0), sourced
  from [dhepper/font8x8](https://github.com/dhepper/font8x8)
  (`font8x8_basic.h`), originally based on IBM VGA ROM font data by Marcel
  Sondaar. No code from that project is used — only the glyph bitmap data,
  reformatted into our own header/source split.

Everything else (kernel, drivers, GUI, build system) is original code
written for this project. GRUB is used only as a bootloader (Multiboot2
loader) — it is a separate, non-Linux GPLv3 project and is not linked into
or shipped inside the kernel binary; it only lives in `/boot/grub` on the
ISO, which the kernel never reads back from.

## Roadmap: making this an "everyday OS"

Rough order of what to build next, and concretely how:

1. **Preemptive multitasking** — a task struct + round-robin scheduler
   driven off the existing PIT IRQ0 handler, plus a `context_switch.asm`
   that saves/restores registers and switches `esp`/`cr3`. This unblocks
   everything else (a network stack needs a receive task; real apps need
   isolation).
2. **User mode** — ring-3 task support: a TSS, `iret`-based ring transitions,
   a syscall interrupt (e.g. `int 0x80`) for controlled kernel entry.
3. **Filesystem** — an ATA PIO (or AHCI) disk driver, then a simple
   filesystem (FAT32 is the pragmatic choice: well-documented, and lets
   you exchange files with a real OS by mounting the disk image).
4. **Networking** — PCI enumeration (scaffolding for this is a natural
   next addition to `drivers/`) to find a NIC, a driver for a
   well-documented, QEMU-friendly chip (RTL8139 or the Intel E1000 — both
   have public datasheets and are the standard hobby-OS starting point),
   then a minimal TCP/IP stack (ARP → IP → ICMP/UDP → TCP) — ping working
   is the first real milestone, DHCP + a TCP client after that.
5. **Audio** — an AC97 or Intel HDA driver (AC97 is simpler and what QEMU's
   `-device AC97` emulates), PCM playback via DMA buffers.
6. **A real windowing API** — right now windows are hardcoded in
   `gui/compositor.c`; the next step is a message-passing API so user-mode
   processes can create/draw into their own windows, which is what turns
   this from "one big demo GUI" into an actual application platform.

Each of these is independently a multi-day-to-multi-week task; happy to
keep building on any of them next.
