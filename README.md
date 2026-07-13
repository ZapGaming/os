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
  TX descriptors), and a real stack built up in layers — Ethernet, ARP
  (request/reply, with a cache), IPv4 (with header checksums *and*
  gateway routing for off-subnet destinations), ICMP echo, UDP, a DNS
  resolver (A records, over UDP/53), and a client-only TCP (active-open
  connect/send/recv/close with a real handshake and stop-and-wait
  retransmission). A background task resolves the gateway and pings it
  once a second; the GUI's "Network" window shows the NIC's real MAC,
  our IP, the resolved gateway, and live ping stats. Verified against a
  real packet capture (see "How networking works" below) — the gateway's
  replies genuinely round-trip.
- **A real web browser with a CSS box-model layout engine**: an HTTP/1.1
  client on top of TCP (handles both `Content-Length` and chunked
  transfer-encoding), a real DOM tree parser (`net/dom.c`), a CSS parser
  and cascade (`net/css.c`, a UA default stylesheet plus a page's own
  `<style>` blocks and inline `style=""`, with real property
  inheritance), and a layout engine (`net/layout.c`) that walks the DOM
  with resolved styles into block/inline boxes — real vertical margins
  and padding, block-level background colors, `<hr>` rules, list-item
  bullets, and per-word wrapped, individually-colored text runs. Links
  are genuinely clickable: clicking one resolves the href (relative,
  absolute-path, or absolute-URL) against the current page and
  navigates. The GUI's "Browser" window has a real address bar (with
  optional `host:port`); press Enter and it resolves DNS, opens a TCP
  connection, fetches the page, and lays it out. Verified end-to-end
  against both a real, live website and a local multi-page CSS test
  site (see "How the browser works" below).
- **Filesystem**: an ATA PIO disk driver and a real FAT32 driver (BPB
  parsing, FAT-chain walking, directory listing, file read *and* write)
  on a separate 64MB disk image. The GUI's "File Manager" window browses
  it live — click a folder to navigate in, click a file to preview it,
  and `NOTES.TXT` is actually editable: type into it, press Enter to
  save, and it persists across a full reboot (verified end-to-end,
  including through the GUI itself — see "How the filesystem works").
- **Serial debug console** (COM1) for early boot logging — see it with
  `make run` or `-serial stdio`.

## What's stubbed / not yet built

- **Audio**: no sound driver yet.
- **The browser's CSS support is a pragmatic subset, not real CSS**: no
  horizontal box model (no width/height/floats/inline-block, no
  centering or horizontal margins — only vertical stacking with a fixed
  left indent), no tables/images, no descendant/child selectors (only
  bare tag, `.class`, `#id`, and `tag.class`), no `<style>` media
  queries. There's no HTTPS (plain HTTP only — no TLS, so `https://`
  links are refused rather than fetched), the fetch is synchronous and
  blocks GUI redraws while it runs, and only one TCP connection can be
  open at a time (no fetching a page and its images concurrently).
- **Filesystem writes are constrained**: `fat32_write_file` can only
  overwrite a file that already exists in a directory (it doesn't create
  new directory entries or grow a directory) — the shipped disk image
  pre-creates `NOTES.TXT` as an empty placeholder for exactly this reason.
- **No DHCP**: the IP config is static, matching QEMU's default SLIRP
  network so `make run` just works. Incoming packet checksums aren't
  validated (outgoing ones are computed correctly). TCP is client-only
  (active-open) — there's no listening/server side.
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

## How the browser works

`net/udp.c` adds a small port-based dispatch table (`udp_register_handler`)
on top of IPv4; `net/dns.c` uses it to send an A-record query to QEMU
SLIRP's built-in resolver (`<gateway-subnet>.3`, e.g. `10.0.2.3`) and
blocks (with a timeout, via `pit_ticks()`) waiting for the reply,
including handling DNS name compression pointers in the response.
`net/tcp.c` is a single-connection, client-only, active-open state
machine (`SYN_SENT` → `ESTABLISHED` → `FIN_WAIT1/2` → `LAST_ACK`) with
stop-and-wait retransmission — send a segment, wait for its ACK before
sending the next one, no windowing or congestion control. `net/http.c`
drives that to do an HTTP/1.1 GET, and understands both `Content-Length`
and chunked transfer-encoding responses.

From there, three layers turn the raw HTML bytes into pixels:

- **`net/dom.c`** parses the HTML into a real tree (kmalloc'd nodes with
  a tag, `id`/`class`/`href`/inline-`style` attributes, and text-node
  children) instead of a flat line list — a real (if minimal) DOM.
- **`net/css.c`** parses a pragmatic CSS subset: a hardcoded UA default
  stylesheet (block/inline defaults, heading/link/bold colors, list and
  blockquote spacing) plus whatever the page's own `<style>` blocks and
  inline `style=""` attributes add, resolved with real property
  inheritance and a simple cascade (later rules win per-property; inline
  style wins over everything).
- **`net/layout.c`** walks the DOM with resolved styles into a flat list
  of positioned render items in document-pixel space: block children
  stack vertically with real margins/padding, inline content (text,
  `<a>`/`<b>`/`<span>`) flows and word-wraps within the current block's
  width, and each *word* becomes its own item with its own color and
  link id — which is what makes individual links genuinely clickable
  (hit-testing is just "does the click point fall inside this word's
  box"), not merely styled.

The GUI's Browser window (`gui/compositor.c`) draws that item list in
three passes (backgrounds, then rules, then text, so a block's own
background never paints over its text regardless of emission order),
and clicking a link resolves its `href` against the current URL
(handling `http://` absolute, `//host/path`, `/path` absolute-path, and
plain relative hrefs, with `#fragment`/`mailto:`/`javascript:` treated as
no-ops and `https://` refused outright — no TLS client exists) before
re-fetching.

Several real bugs surfaced while getting this to render correctly
end-to-end — all caught by actually fetching real pages (both a live
site and a small local multi-page CSS test site) rather than trusting
code review:

1. **DNS silently failed.** `ip_send()` originally only checked the ARP
   *cache* for the next hop and gave up if it missed — fine for the
   gateway (which gets ARP'd by the ping task) but nothing had ever
   ARP'd the DNS server's IP, so every DNS query silently failed to even
   go out. Fixed with `ip_resolve_next_hop()`, which actively sends ARP
   *requests* and retries (5 attempts, 200ms apart) before giving up.
   The same fix made off-subnet routing work in general: `ip_send()` now
   routes through the gateway for any destination outside our `/24`.
2. **A whole class of tags silently rendered as inline text.** The UA
   default stylesheet's `display: block` rule lists ~25 tag names in one
   comma-separated selector (`h1,h2,...,blockquote,...,form`), but
   `css_parse_into` only kept the first 4 comma-separated selectors per
   rule (`CSS_MAX_GROUPS` was 4). Every tag past the 4th silently never
   matched that rule, so `h1`, `ul`, `li`, `hr`, `blockquote`, and more
   all defaulted to `display: inline` — no margins, no backgrounds, no
   line breaks between them, everything ran together as one paragraph.
   Caught by literally walking the DOM tree with debug logging and
   noticing those tags were visited but never reached the block-layout
   code path. Fixed by raising `CSS_MAX_GROUPS` to 32.
3. **The fix above appeared to do nothing at first** — because the
   Makefile's `%.o: %.c` rule has no header dependency tracking, so
   editing `css.h` didn't trigger a rebuild of anything that includes
   it. `make` silently relinked the *old* object file. Fixed by adding
   `-MMD -MP` to `CFLAGS` and `-include`ing the generated `.d` files, so
   header edits now correctly invalidate their dependents. This wasn't
   a browser bug, but it's the kind of thing that makes a real bug look
   fixed when it isn't, so it's worth calling out.
4. **Unstyled pages were invisible.** The UA default text color was a
   light near-white (left over from when the reader-mode renderer only
   ever painted on the OS's own dark window background). Once real CSS
   backgrounds started rendering, a page with a light `background-color`
   but no explicit `color` (e.g. `example.com`, which relies on the
   browser default) produced near-white text on a near-white
   background — unreadable. Real browsers default to *black* text on a
   light canvas; fixed by changing the UA default to black and giving
   the Browser window's own content area a light default fill (drawn
   before any page-supplied background), matching that same convention.

Two smaller bugs were fixed in the reader-mode-era code and are now
moot since that renderer (`net/html.c`) was deleted and fully replaced
by the DOM/CSS/layout pipeline above: a style-flush-ordering bug and a
line-truncation bug in the old flat HTML parser.

## How the filesystem works

`drivers/ata.c` drives the primary IDE channel with plain PIO (IDENTIFY,
then 28-bit LBA read/write) and deliberately only looks at the primary
*master* — the boot CD-ROM is an ATAPI device and gets skipped by
checking the IDENTIFY signature. `fs/fat32.c` reads the BPB from LBA 0
(the companion disk image has no partition table — it's a FAT32
"superfloppy," so the filesystem starts right at sector 0), then
implements the FAT32 essentials from scratch: cluster↔LBA math, walking
a 32-bit FAT chain, directory-entry parsing (8.3 names only — long
filename entries are skipped, not decoded), reading a file's cluster
chain, and writing one (allocate free clusters by scanning the FAT,
chain them, write the data, update the existing directory entry).

`tools/make_disk_image.sh` builds `zapos_disk.img` (and a `.vmdk`
alongside it for VMware/VirtualBox) with `mtools` — no root or loop
devices needed. It ships `README.TXT`, `DOCS/ABOUTFS.TXT`, and an empty
`NOTES.TXT` that the File Manager can actually edit and save.

Getting this right needed one more fix on top of everything already
running: with a hard disk attached, the BIOS's default boot order tries
the hard disk *before* the CD-ROM, and since `zapos_disk.img` has no
boot code on it, that's a silent hang before any of our own code even
runs. `-boot order=d` (CD-ROM first) fixes it — `make run` and the
troubleshooting notes below both account for this.

## Testing in other VMs (VMware, VirtualBox, browser-based emulators)

ZapOS is a completely standard Multiboot2 kernel on a standard El Torito
bootable ISO, so it isn't tied to QEMU. It was also verified booting
correctly under [v86](https://github.com/copy/v86) (a WASM x86 emulator
used by some browser-based "run an OS in a tab" tools) — GUI, scheduler,
ring-3, and syscalls all confirmed working there identically to QEMU. Two
things to know if you hit trouble in a different VM:

- **Give it a boot CD/DVD, not a floppy or generic disk**, and if you
  also attach `zapos_disk.img`/`zapos_disk.vmdk`, make sure the CD-ROM is
  first in boot order (see above) — otherwise it'll try to boot from the
  data disk and hang.
- **Don't over-allocate RAM.** ZapOS itself needs only a few MB. One
  browser-based emulator tested here would reliably crash *itself*
  (inside its own BIOS-loading code, before ZapOS ever runs) when given
  4GB of guest RAM, but worked perfectly at 256MB — if a VM tool offers
  a RAM slider or "compatibility" preset, prefer the smaller option.
- Networking needs an **RTL8139** NIC specifically (that's the only
  driver written so far) attached with a network backend that actually
  answers ICMP (QEMU's user-mode/SLIRP networking does this by default).
  Some tools don't attach a NIC at all — ZapOS handles that gracefully
  (the Network window just shows "no NIC detected"), it's not an error.

## Building and running

Requires: `gcc` (with 32-bit multilib support), `nasm`, `grub-mkrescue`,
`xorriso`, `qemu-system-x86` (all installed via apt in this environment).

```sh
make          # compile the kernel (build/kernel.elf)
make iso      # package it as zapos.iso via GRUB
make disk     # build zapos_disk.img + zapos_disk.vmdk (only if missing --
              # won't clobber anything you've saved via the File Manager)
make run      # build both and boot in QEMU with a NIC + disk + serial on stdio
```

`make run` attaches an RTL8139 NIC via QEMU's user-mode (SLIRP) networking
and the FAT32 disk image, with `-boot order=d` so it boots the CD-ROM
first (see above for why that matters once a hard disk is attached).
Booting `zapos.iso` some other way (VirtualBox/VMware/real hardware, or
without `zapos_disk.img` at all) works fine too — the GUI just shows "no
NIC detected" / "no disk/FAT32 detected" for whichever piece isn't
present, and everything else runs the same. Boot mode is legacy BIOS
(not UEFI/Secure Boot yet — that would need a `grub-mkrescue --efi` build
and a different Multiboot path).

## Architecture / directory layout

```
boot/            multiboot2 header + real assembly entry point
kernel/          GDT/IDT/ISR/IRQ, PIC, PIT, paging, physical memory
                 manager, kernel heap, multiboot info parser, serial console,
                 scheduler + context switch, TSS, ring-3 entry, syscalls
drivers/         PS/2 controller, keyboard, mouse, PCI enumeration, RTL8139 NIC, ATA
gui/             framebuffer primitives, bitmap font, window compositor
net/             Ethernet, ARP, IPv4, ICMP, UDP, DNS, TCP, HTTP -- a
                 from-scratch TCP/IP stack -- plus DOM/CSS/layout, a
                 real (if pragmatic) web browser backend
fs/              FAT32 driver (BPB, FAT chains, directory listing, read/write)
include/         public headers, mirroring kernel/, drivers/, gui/, net/, fs/
linker.ld        places the kernel at 1 MiB physical/virtual (identity-mapped)
Makefile         freestanding i386 build (gcc -m32 -ffreestanding -nostdlib)
iso/grub.cfg     GRUB menu entry (multiboot2 /boot/kernel.elf)
tools/make_disk_image.sh  builds the companion FAT32 disk image (mtools, no root needed)
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

Preemptive multitasking, ring-3 user mode, syscalls, a full
Ethernet/ARP/IPv4/ICMP/UDP/DNS/TCP stack, a real read/write FAT32
filesystem, and a web browser with a real (if pragmatic) CSS box-model
layout engine and clickable links are now done (see above). Rough order
of what's next:

1. **Per-process page directories** — give each task its own CR3 instead
   of sharing one identity-mapped 4 GiB space. This is what turns "ring-3
   mechanics work" into "processes are actually isolated," and is a
   prerequisite for loading untrusted code (like a future browser binary)
   safely.
2. **Loading programs from disk** — right now every task is compiled
   into the kernel image; with a filesystem and per-process page
   directories both in place, the natural next step is a minimal ELF
   loader plus `fork`/`exec`-style syscalls, so user programs can be
   files on `zapos_disk.img` instead of demo functions in `kernel.c`.
3. **HTTPS** — a TLS client is a substantial project on its own
   (certificate parsing/validation, at minimum a static-RSA or ECDHE
   cipher suite), but it's the single biggest thing keeping the browser
   from reaching most of the real web.
4. **DHCP + concurrent connections** — replace the static IP config with
   a real DHCP handshake, and lift TCP's single-static-connection
   limitation so multiple sockets can be open at once (needed before the
   browser can, e.g., fetch a page and its images concurrently, or fetch
   asynchronously without blocking GUI redraws).
5. **A real windowing API** — right now windows are hardcoded in
   `gui/compositor.c`; user-mode processes (the eventual browser
   included) need a message-passing syscall API to create/draw into
   their own windows rather than being baked into the compositor.
6. **Audio** — an AC97 or Intel HDA driver (AC97 is simpler and what QEMU's
   `-device AC97` emulates), PCM playback via DMA buffers.

Each of these is independently a multi-day-to-multi-week task; happy to
keep building on any of them next.
