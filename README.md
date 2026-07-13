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
- **Preemptive multitasking**: a real scheduler with independent kernel
  stacks per task, driven off the PIT timer interrupt — tasks are switched
  transparently, not cooperatively. See "How the scheduler works" below.
- **Ring-3 user mode**: a TSS and an `iret`-based ring0→ring3 transition.
  A demo task genuinely executes at CPL 3 (privileged instructions like
  `in`/`out` would fault there) and runs alongside the GUI and the kernel
  tasks.
- **Syscalls**: an `int 0x80` gate (DPL 3, so ring-3 code can invoke it)
  with `write`/`yield`/`exit` — the only way ring-3 code can talk to the
  kernel. The GUI's "Process Monitor" window shows this live: task count,
  a live-incrementing counter from a background kernel task, and the
  actual string the ring-3 task sent via syscall.
- **Networking**: PCI enumeration, an RTL8139 driver (IRQ-driven RX ring +
  TX descriptors), and a real minimal stack — Ethernet, ARP (request/reply,
  with a cache), IPv4 (with header checksums), and ICMP echo. A background
  task resolves the gateway and pings it once a second; the GUI's
  "Network" window shows the NIC's real MAC, our IP, the resolved gateway,
  and live ping stats. Verified against a real packet capture (see "How
  networking works" below) — the gateway's replies genuinely round-trip.
- **Serial debug console** (COM1) for early boot logging — see it with
  `make run` or `-serial stdio`.

## What's stubbed / not yet built

- **Audio**: no sound driver yet.
- **Filesystem**: no on-disk filesystem or persistent storage driver (ATA/AHCI).
- **Networking is Ethernet/ARP/IPv4/ICMP only**: no UDP or TCP yet, no DHCP
  (the IP config is static, matching QEMU's default SLIRP network so
  `make run` just works), and incoming packet checksums aren't validated
  (outgoing ones are computed correctly).
- **Real process isolation**: every task (kernel and ring-3) shares the
  same identity-mapped address space — there's no per-process page
  directory yet, so a user task *could* read/write kernel memory or
  another task's memory. The ring-3 mechanics (privilege transitions,
  syscalls, faulting on privileged instructions) are real; the memory
  protection between processes is not, yet.
- **Process lifecycle**: exited tasks are marked terminated and skipped by
  the scheduler, but their stack memory is never freed, and there's no
  `wait()`/parent-child relationship, exit codes, or process reaping.

See "Roadmap" below for how each of these would actually get built.

## How the scheduler works

Each task (`struct task` in `kernel/scheduler.c`) has its own kernel
stack. `switch_task` (`kernel/switch_task.asm`) is the whole mechanism:
it saves the callee-saved registers of the current task onto its own
stack, records that stack pointer, then loads the next task's stack
pointer and restores its registers. The final `ret` resumes execution
wherever that task last left off — because that's exactly what's sitting
on top of its stack.

The scheduler is invoked from inside the PIT timer interrupt handler
(`schedule()` is registered via `pit_set_tick_callback`). One critical
detail: the interrupt's EOI is sent to the PIC *before* the handler runs
(see the comment in `kernel/idt.c`), not after — because a task switch
means control never unwinds back to the normal interrupt epilogue for the
task being switched away from; it resumes some *other* task's previously
suspended call chain instead. Sending EOI early was the single trickiest
bug in this milestone: without it, the PIC considers the first timer
interrupt permanently "in service" and never delivers IRQ0 again, silently
freezing the whole system after exactly one task switch.

Brand-new tasks are bootstrapped with a fake initial stack frame (built in
`task_create`) that makes `switch_task`'s `ret` land in
`task_trampoline.asm`, which re-enables interrupts (a real interrupt
return via `iret` would have restored `EFLAGS.IF` automatically, but a
freshly created task has never gone through that path) and then calls the
task's real entry point. Ring-3 tasks route through an extra `user_task_shim`
that calls `enter_usermode.asm` to `iret` into CPL 3 with a separate user
stack; the kernel stack is kept around too, since the CPU needs it (via
the TSS's `esp0`) the moment that task takes an interrupt or syscall back
into ring 0.

## How networking works

`drivers/pci.c` enumerates the PCI bus (legacy 0xCF8/0xCFC config-space
access) to find the RTL8139 (vendor 0x10EC, device 0x8139 — what QEMU's
`-device rtl8139` emulates). `drivers/rtl8139.c` resets it, gives it a
receive ring buffer and four transmit descriptor slots, and hooks its PCI
interrupt line. `net/` layers Ethernet → ARP → IPv4 → ICMP on top, each in
its own file, dispatching by ethertype/protocol number. The IP config
(`10.0.2.15`, gateway `10.0.2.2`) is static and matches QEMU SLIRP's
defaults, so there's no DHCP client yet.

Verifying this actually worked took a real packet capture (`tcpdump -r`
on a `-object filter-dump` pcap), which is worth calling out because it
caught two real concurrency bugs that pure code review missed — both are
consequences of preemption happening on *every* PIT tick, unconditionally:

1. **A frozen system, again.** `task_exited()` (used for both a normal
   ring-3 `exit` syscall and a kernel task returning normally) loops
   forever and never reaches an `iret`. But `switch_task` only saves/
   restores four callee-saved registers — never `EFLAGS`. Every task that
   was ever interrupted by a real hardware interrupt has `IF=1` baked into
   its own saved trapframe, restored automatically when it eventually
   `iret`s. A task stuck in `task_exited()`'s loop never does that, so it
   permanently carries whatever `IF` happened to be when the interrupt
   gate that got it there was entered — 0. The first time that zombie task
   got rescheduled, it ran, hit its own `hlt` with interrupts disabled,
   and froze the machine for good (a real timer tick that had already
   fired stayed masked forever, since `hlt` with `IF=0` only wakes on an
   NMI). Fixed with an explicit `sti` at the top of `task_exited()`.
2. **Pings that should have worked, silently didn't.** A packet capture
   showed the gateway replying correctly, with matching IDs and sequence
   numbers, in well under a millisecond — faster than our own sender task
   could get scheduled back in to finish updating its own bookkeeping.
   `icmp_send_echo_request` recorded "reply we're expecting" *after*
   calling `ip_send()`; the RX interrupt for the reply (handled
   independently of whichever task is current) could get serviced first,
   see `pending_outstanding` still 0, and silently drop a perfectly valid
   reply. Fixed by setting that state *before* sending.

## Building and running

Requires: `gcc` (with 32-bit multilib support), `nasm`, `grub-mkrescue`,
`xorriso`, `qemu-system-x86` (all installed via apt in this environment).

```sh
make          # compile the kernel (build/kernel.elf)
make iso      # package it as zapos.iso via GRUB
make run      # build the ISO and boot it in QEMU with a NIC + serial on stdio
```

`make run` attaches an RTL8139 NIC via QEMU's user-mode (SLIRP) networking
(`-netdev user -device rtl8139`) so ping-the-gateway works out of the box.
Booting `zapos.iso` some other way (VirtualBox/VMware/real hardware) works
fine without a NIC too — the GUI's Network window just shows "no NIC
detected" and everything else runs the same. Boot mode is legacy BIOS
(not UEFI/Secure Boot yet — that would need a `grub-mkrescue --efi` build
and a different Multiboot path).

## Architecture / directory layout

```
boot/            multiboot2 header + real assembly entry point
kernel/          GDT/IDT/ISR/IRQ, PIC, PIT, paging, physical memory
                 manager, kernel heap, multiboot info parser, serial console,
                 scheduler + context switch, TSS, ring-3 entry, syscalls
drivers/         PS/2 controller, keyboard, mouse, PCI enumeration, RTL8139 NIC
gui/             framebuffer primitives, bitmap font, window compositor
net/             Ethernet, ARP, IPv4, ICMP -- a minimal from-scratch TCP/IP stack
include/         public headers, mirroring kernel/, drivers/, gui/, net/
linker.ld        places the kernel at 1 MiB physical/virtual (identity-mapped)
Makefile         freestanding i386 build (gcc -m32 -ffreestanding -nostdlib)
iso/grub.cfg     GRUB menu entry (multiboot2 /boot/kernel.elf)
```

The whole 4 GiB address space is identity-mapped (no higher-half kernel,
no per-process page directories yet), and there are currently up to four
concurrently scheduled tasks: the boot/GUI task (ring 0), a background
counter task (ring 0), a demo task that runs at ring 3 and talks to the
kernel only via `int 0x80` syscalls, and (if a NIC is present) the ping
task. The GUI's own event loop (`gui_run()`) is itself just one of these
tasks — it polls the mouse/keyboard and redraws at roughly 60 fps via a
PIT-timed sleep, same as before, but now it's preemptible rather than the
only thing running.

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

Preemptive multitasking, ring-3 user mode, syscalls, and basic networking
(Ethernet/ARP/IPv4/ICMP, ping working) are now done (see above). Rough
order of what's next, and concretely how:

1. **Per-process page directories** — give each task its own CR3 instead
   of sharing one identity-mapped 4 GiB space. This is what turns "ring-3
   mechanics work" into "processes are actually isolated," and is a
   prerequisite for loading untrusted code safely.
2. **Filesystem** — an ATA PIO (or AHCI) disk driver, then a simple
   filesystem (FAT32 is the pragmatic choice: well-documented, and lets
   you exchange files with a real OS by mounting the disk image). Combined
   with #1, this is what lets user programs be loaded from disk instead
   of compiled into the kernel image as demo tasks.
3. **UDP + TCP + DHCP** — the Ethernet/ARP/IPv4/ICMP foundation is there;
   UDP is a small addition (no connection state), TCP is the real work
   (connection state machine, retransmission, windowing), and DHCP would
   replace the current static IP config with a real handshake.
4. **Audio** — an AC97 or Intel HDA driver (AC97 is simpler and what QEMU's
   `-device AC97` emulates), PCM playback via DMA buffers.
5. **A real windowing API** — right now windows are hardcoded in
   `gui/compositor.c`; the next step is a message-passing syscall API so
   user-mode processes can create/draw into their own windows, which is
   what turns this from "one big demo GUI" into an actual application
   platform. More syscalls generally (`fork`/`exec`-equivalents once #1
   and #2 land, proper process exit/reaping) fall under this too.

Each of these is independently a multi-day-to-multi-week task; happy to
keep building on any of them next.
