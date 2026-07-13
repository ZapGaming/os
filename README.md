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
  navigates. Fetching something that *isn't* HTML (by `Content-Type`,
  or the URL's extension as a fallback) doesn't try to render it — it
  gets saved straight to the FAT32 disk instead, so a song, video, or
  any other file you point the browser at ends up as a real file in the
  File Manager. The GUI's "Browser" window has a real address bar (with
  optional `host:port`); press Enter and it resolves DNS, opens a TCP
  connection, fetches, and either lays out or downloads. Verified
  end-to-end against both real, live websites and a local multi-page
  CSS test site (see "How the browser works" below).
- **A real JavaScript engine**: a from-scratch lexer, recursive-descent
  parser, and tree-walking interpreter (`js/`) for a pragmatic ES5-ish
  subset — variables (`var`/`let`/`const` with real block scoping),
  functions with genuine closures, `if`/`for`/`while`, all the standard
  operators, array/object literals — wired to a minimal but real DOM API
  (`document.getElementById`, `element.textContent`/`innerHTML` get and
  set, `element.style.property = ...`, `element.onclick = fn` with
  event bubbling, `console.log`, `Math`). Inline `<script>` bodies run
  once after the page's DOM is parsed but before its first layout (so
  a script can build page content before anything is ever drawn), and
  clicking an element re-invokes its registered handler and re-lays-out
  the page live if the handler mutated the DOM — a JS-driven counter
  button and a click-to-recolor button both work for real, closures
  and all (see "How the JS engine works" below). Numbers are 32-bit
  integers only, not IEEE754 doubles — this kernel is built with
  `-mno-80387 -mno-sse`, so no FPU/SSE state is ever initialized and
  floating point genuinely cannot be generated anywhere in it.
- **Audio**: a real AC97 codec driver (`drivers/ac97.c`, bus-master DMA
  via a descriptor list, matching what QEMU's `-device AC97` emulates)
  and a WAV file parser (`drivers/wav.c`). The File Manager can play any
  16-bit/48kHz WAV file on the disk (P to play, S to stop) — genuinely
  through the hardware's DMA engine, not a software mixer. See "How
  audio works" below for how this was actually verified (QEMU's default
  "no backend" audio setup never exposed a real bug that a proper
  backend did) and its real limits (WAV only — no MP3/video decoding of
  any kind exists, or is realistically in scope for a from-scratch OS
  built solo).
- **Filesystem**: an ATA PIO disk driver and a real FAT32 driver (BPB
  parsing, FAT-chain walking, directory listing, file read/write *and*
  creating brand-new files — allocating a free directory entry, or
  growing the directory by a cluster if it's full) on a separate 64MB
  disk image. The GUI's "File Manager" window browses it live — click a
  folder to navigate in, click a file to preview it (or play it, for
  audio), and `NOTES.TXT` is actually editable: type into it, press
  Enter to save, and it persists across a full reboot (verified
  end-to-end, including through the GUI itself — see "How the
  filesystem works"). The listing also shows a live item count and
  color-codes/tags entries by type (directories, `.WAV` audio, plain
  files).
- **Serial debug console** (COM1) for early boot logging — see it with
  `make run` or `-serial stdio`.

## What's stubbed / not yet built

- **The JS engine is a pragmatic ES5-ish subset, not real JavaScript**:
  no prototypes/classes, no `try`/`catch`, no template literals, no
  `for-in`/`for-of`, no destructuring, no arrow functions, no
  `Promise`/`async`/`await`, no `setTimeout`/`setInterval` (there's no
  event loop at all beyond "a click runs a handler synchronously"), and
  no `<script src="...">` fetching (only inline `<script>` bodies run —
  an external one is detected and skipped with a log line, never
  fetched). No garbage collector either: everything a page's scripts
  allocate lives in one fixed 256KB arena that's thrown away whole on
  the next navigation, so a script that allocates enough across many
  repeated clicks without ever navigating away could exhaust it (logged
  to serial, not a crash — allocation just starts silently no-opping).
  Numbers are 32-bit integers, not doubles (see above) — arithmetic
  that would produce a fraction in real JS just truncates.
- **No video or compressed-audio playback of any kind**: the browser can
  *download* a `.mkv`/`.mp3`/anything to disk, but nothing on this OS
  can decode video or compressed audio codecs — that's a multi-year
  codec-engineering effort even for established projects, and genuinely
  out of scope here. WAV (uncompressed PCM) is the only playable format.
- **The browser's CSS support is a pragmatic subset, not real CSS**: no
  horizontal box model (no width/height/floats/inline-block, no
  centering or horizontal margins — only vertical stacking with a fixed
  left indent), no tables/images, no descendant/child selectors (only
  bare tag, `.class`, `#id`, and `tag.class`), no `<style>` media
  queries. There's no HTTPS (plain HTTP only — no TLS, so `https://`
  links are refused rather than fetched), the fetch is synchronous and
  blocks GUI redraws while it runs, and only one TCP connection can be
  open at a time (no fetching a page and its images concurrently).
  Downloads are capped at just under 2MB (the whole file has to fit in
  memory at once — no streaming-to-disk) and derive their saved filename
  from the URL path verbatim (no percent-decoding).
- **Filesystem writes still can't delete or rename** — `fat32_write_file`
  can create and overwrite, but there's no way to remove a directory
  entry or grow a file's directory *tree* (only its own cluster chain
  and, for the immediate parent, one additional directory cluster).
- **No DHCP**: the IP config is static, matching QEMU's default SLIRP
  network so `make run` just works. Incoming packet checksums aren't
  validated (outgoing ones are computed correctly). TCP is client-only
  (active-open), single-connection (no concurrent sockets) — there's no
  listening/server side.
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
stop-and-wait retransmission for its *own* sends (write a segment, wait
for its ACK before sending the next one — no congestion control), and a
real (if fixed-size, 32KB) receive window that's actually advertised
and enforced on the receive side — see bugs #5/#6 below for what that
took to get right. `net/http.c` drives the connection to do an HTTP/1.1
GET, and understands both `Content-Length` and chunked
transfer-encoding responses, plus (now) the `Content-Type` header,
which is what the browser uses to decide whether to render or download
a response.

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

Adding downloads (fetching anything non-HTML straight to disk) surfaced
three more real bugs, all in code that pre-dated this pass but had
never been exercised by a transfer bigger than a small HTML page:

5. **Downloads over ~32KB came back silently truncated.** In
   `tcp_handle_packet()`'s `TCP_ESTABLISHED` case, when an incoming
   segment was bigger than the free space left in the 32KB receive
   buffer, the code stored only what fit (`copy_len`) but advanced
   `recv_seq` — and therefore what the ACK it sent claimed to have
   received — by the *full* segment length. That silently acknowledged
   bytes that were never actually stored anywhere, so they were gone
   for good instead of being retransmitted once space freed up. Fixed
   by only advancing `recv_seq` by `copy_len`.
6. **Fixing #5 turned truncation into a permanent hang.** Once the
   receive buffer legitimately filled up, both sides got stuck: the
   real sender was still being told (via a *hardcoded* `window=8192` on
   every segment we sent, success or not) that we always had room,
   so it kept trying to push data we had nowhere to put; meanwhile we
   had no way to tell it "wait" or "you can resume now" once we'd
   drained space, because the window field never reflected our actual
   buffer occupancy. Fixed by computing `hdr->window` from real free
   space (`TCP_RECV_BUF_SIZE - conn.recv_len`) on every segment, so it
   correctly drops toward 0 as the buffer fills and rises again once
   `tcp_recv()` drains it — real TCP flow control instead of a constant
   that happened to work only because nothing had ever filled the
   buffer before.
7. **A large download's failure was completely silent.** Bumping
   `http.c`'s receive buffer to fit bigger responses meant it, the
   browser's own fetch buffer, and anything already loaded (e.g. a WAV
   file open in the File Manager) could collectively exceed the 8MB
   kernel heap — and the lazy `kmalloc()` for that buffer had no log
   line on failure, so `http_get()` just returned 0 with zero
   indication why. This is what actually cost the most time to track
   down (bug #5/#6 above hid behind it at first). Fixed by sizing both
   buffers to comfortably coexist (2MB each) and logging the
   out-of-memory case.

## How the JS engine works

`js/lexer.c` tokenizes source into numbers (integers only — see the
FPU note above), strings (with the common escapes), identifiers/
keywords, and operators (longest-match-first, so `===` doesn't get
split into `==` `=`). `js/parser.c` is a straightforward recursive-
descent parser with one function per precedence level (assignment →
conditional → `||` → `&&` → equality → relational → additive →
multiplicative → unary → postfix → primary) producing an AST
(`js.h`'s `struct js_node`, a tagged union). `js/interp.c` walks that
AST against a real scope chain (`struct js_env`, one per function call
and per block — `var` hoists to the nearest function/global scope,
`let`/`const` are block-scoped) with proper control-flow propagation
for `return`/`break`/`continue` threaded back up through nested
statements. Closures work because a function value just carries a
pointer to the `js_env` that was active when it was defined
(`fn->closure_env`), and calling it later builds a fresh call-local env
with *that* as its parent — the classic tree-walking-interpreter
closure trick.

Everything the interpreter allocates (AST nodes, strings, objects,
environments) comes out of one 256KB bump-allocated arena
(`js_alloc()` in `js/value.c`) that's simply thrown away and
re-created on the next page navigation — there's no garbage collector,
and there doesn't need to be one, since nothing a page's scripts
create needs to outlive that page.

`js/dom_binding.c` is the only part that knows about `net/dom.h`: it
wraps a `dom_node*` as a `JS_OBJ_DOM_ELEMENT` value, implements
`document.getElementById` by walking the live DOM tree, and gives
`element.style` its own object kind (`JS_OBJ_DOM_STYLE`) whose
property *setters* rewrite the element's inline style text in place
(converting `backgroundColor` → `background-color` generically, not
via a lookup table) rather than storing arbitrary JS values — so
`this.style.backgroundColor = "..."` genuinely flows through the same
CSS cascade every other background color does. `onclick` handlers are
kept in a small side table keyed by `dom_node*` (a fresh
`JS_OBJ_DOM_ELEMENT` wrapper gets created every time JS code reads
`document.getElementById(...)`, so the handler can't live *on* that
wrapper object — it has to be keyed by the stable node pointer
instead). Dispatching a click walks up the clicked element's `parent`
chain looking for a registered handler (simple bubbling: first handler
found wins, no `stopPropagation`), which is also why `net/dom.c` grew
a `parent` field on `dom_node` in this pass.

The browser (`gui/compositor.c`) now keeps a page's DOM tree and
resolved stylesheet alive for as long as it's displayed (previously
both were freed the instant the first layout finished, since nothing
needed them afterward) so that a click can mutate the live tree and
`br_relayout()` can re-run `layout_run()` against the *same* tree
afterward. Making a specific element clickable at all needed the
layout engine to remember, for every rendered word, which DOM element
it actually belongs to — a small generalization of the link-hit-testing
threading that already existed for `<a>` tags (`net/layout.c`'s
`layout_children` already threaded a `link_id` down through recursion;
this pass added an `owner` pointer alongside it, updated to the current
element at each element boundary during the walk).

Two real, and one very educational, bugs came out of building this:

1. **A `<div>`'s own background color never showed up if any ancestor
   (even `<body>`) also had one.** `net/layout.c` originally appended
   each block's background rect to the render-item list *after*
   laying out its children (since the rect's height isn't known until
   then) — which meant a parent's rect always landed at a *later*
   array index than its children's rects, and the draw loop paints a
   whole pass front-to-back in array order. A later index means
   "painted on top," so any container with its own background
   (`<body>` in particular, since nearly every real page's `<body>`
   sets one) silently painted over every background nested inside it.
   This had been live and wrong since the CSS milestone before this
   one — it just never got *caught*, because the one page that
   exercised it used two similarly dark colors that looked fine at a
   glance in a screenshot. It took a test page with two loudly
   different colors (bright blue and purple) to make the bug
   impossible to miss. Fixed by reserving the parent's rect's array
   slot *before* recursing into its children (so it's always earlier,
   and therefore painted first/underneath), then patching its height
   in place once the real value is known.
2. **`&&`/`||` were being parsed as eagerly-evaluated binary operators**,
   not short-circuiting logical ones — the parser's shared precedence-
   climbing helper always built a `JS_BINARY` node regardless of which
   operator table it was given, so `a && b` would evaluate `b`
   unconditionally (and `eval_binary` didn't even have a case for
   `&&`/`||`, so the result would've been wrong regardless). Caught by
   re-reading the parser before ever running it, not by a failing test
   — fixed by having the shared helper take the AST node type to build
   as a parameter, so the logical-operator levels produce real
   `JS_LOGICAL` nodes that the interpreter actually short-circuits.
3. An `ASSIGN` expression node originally stashed its right-hand side
   in the AST's shared `->next` sibling-link field to avoid growing
   the node's union — which silently breaks the moment an assignment
   appears as, say, a function call argument or array element, since
   *those* also use `->next` to link list items and would have
   clobbered (or been clobbered by) the assignment's own RHS pointer.
   Caught the same way as #2 (re-reading before running), fixed by
   giving `JS_ASSIGN` its own `value` field instead of overloading a
   field with an unrelated meaning.

## How audio works

`drivers/ac97.c` finds the Intel ICH AC97 codec via PCI (vendor
`0x8086` device `0x2415` — what QEMU's `-device AC97` emulates), resets
its mixer, sets both volume registers to max, and drives PCM playback
through the codec's bus-master DMA engine: a buffer descriptor list
(up to 32 entries, each up to ~0.68s of 48kHz stereo audio) point
directly at the already-in-RAM PCM samples (no copying needed — the
whole address space is identity-mapped), and starting playback is just
writing the list's address and length into two registers and setting
the "run" bit. `drivers/wav.c` parses a RIFF/WAVE file's `fmt `/`data`
chunks to hand `ac97_play_pcm()` a pointer straight into the file's own
already-loaded bytes. The driver only supports the format that AC97's
"fixed rate" mode actually runs at — 16-bit, 48000Hz — so
`tools/make_disk_image.sh` generates its demo `SONG.WAV` at exactly
that rate.

The one real bug here is worth calling out because *how* it was found
is the interesting part: the driver initially also enabled
`CR_IOCE` (interrupt-on-completion) on the DMA engine, even though no
interrupt handler was ever registered for it. With QEMU's default "no
audio backend" setup (this container has no real sound card), the
timed DMA never actually advanced far enough to trigger it, so testing
looked fine. Only after explicitly attaching a real backend
(`-audiodev wav,...`, to capture what the guest actually played and
confirm it was real, non-silent audio and not just "the driver claims
success") did the DMA genuinely run long enough to hit a buffer
completion — at which point the codec's (level-triggered, and shared
with the RTL8139's PCI IRQ line on QEMU's default chipset) interrupt
line got asserted and never cleared, since nothing was listening for
it. The whole guest froze silently within about one buffer's playback
time. Fixed by simply not enabling `CR_IOCE` at all — the driver polls
`CIV`/`SR` instead of using interrupts, so it never needed to assert
one in the first place. This is a good example of why "the emulator
didn't complain" isn't the same as "the driver is correct" — the bug
was real and hardware-accurate, just invisible until audio was
verified with an actual backend consuming the DMA output.

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
chain them, write the data). Writing looks for a directory entry with
a matching name first (overwrite), then falls back to the first free
slot (a deleted or never-used entry) in an existing directory cluster,
and finally to growing the directory by one more cluster if every
existing slot is already taken — real file *creation*, not just
overwriting a name the disk image happened to ship with, which is what
lets the browser save a download under whatever name the URL gave it.

`tools/make_disk_image.sh` builds `zapos_disk.img` (and a `.vmdk`
alongside it for VMware/VirtualBox) with `mtools` — no root or loop
devices needed. It ships `README.TXT`, `DOCS/ABOUTFS.TXT`, an empty
`NOTES.TXT` that the File Manager can actually edit and save, and a
generated `SONG.WAV` test tone (skipped if `python3` isn't available)
for the audio playback demo.

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
- Audio needs an **AC97** sound device attached. If the VM tool has no
  audio backend configured at all, the AC97 device itself may still not
  even be exposed to the guest depending on the tool — ZapOS handles a
  missing codec gracefully either way (the File Manager just reports
  "no audio device" when you try to play something). Note that a
  *present but backend-less* codec (QEMU with no `-audiodev`, e.g.) can
  behave differently from a real one — see "How audio works" above for
  why that specifically mattered here.

## Building and running

Requires: `gcc` (with 32-bit multilib support), `nasm`, `grub-mkrescue`,
`xorriso`, `qemu-system-x86` (all installed via apt in this environment).
`python3` is optional but recommended — `make disk` uses it to generate
the demo `SONG.WAV`, skipping it gracefully if unavailable.

```sh
make          # compile the kernel (build/kernel.elf)
make iso      # package it as zapos.iso via GRUB
make disk     # build zapos_disk.img + zapos_disk.vmdk (only if missing --
              # won't clobber anything you've saved via the File Manager)
make run      # build both and boot in QEMU with a NIC + AC97 + disk + serial on stdio
```

`make run` attaches an RTL8139 NIC and an AC97 codec via QEMU's default
audio backend, plus the FAT32 disk image, with `-boot order=d` so it
boots the CD-ROM first (see above for why that matters once a hard disk
is attached). Booting `zapos.iso` some other way (VirtualBox/VMware/real
hardware, or without `zapos_disk.img` at all) works fine too — the GUI
just shows "no NIC detected" / "no disk/FAT32 detected" for whichever
piece isn't present, and everything else runs the same. Boot mode is
legacy BIOS (not UEFI/Secure Boot yet — that would need a
`grub-mkrescue --efi` build and a different Multiboot path).

To actually *hear* audio (rather than just verify the driver talks to
the hardware correctly), QEMU needs a real `-audiodev` backend --
`make run`'s default may silently have no backend in a container
without a sound card. Add one explicitly, e.g.
`-audiodev pa,id=snd0 -device AC97,audiodev=snd0` (PulseAudio) or
whatever your host supports in place of the codec-only `-device AC97`.

## Architecture / directory layout

```
boot/            multiboot2 header + real assembly entry point
kernel/          GDT/IDT/ISR/IRQ, PIC, PIT, paging, physical memory
                 manager, kernel heap, multiboot info parser, serial console,
                 scheduler + context switch, TSS, ring-3 entry, syscalls
drivers/         PS/2 controller, keyboard, mouse, PCI enumeration,
                 RTL8139 NIC, ATA, AC97 codec, WAV file parsing
gui/             framebuffer primitives, bitmap font, window compositor
net/             Ethernet, ARP, IPv4, ICMP, UDP, DNS, TCP, HTTP -- a
                 from-scratch TCP/IP stack -- plus DOM/CSS/layout, a
                 real (if pragmatic) web browser backend
fs/              FAT32 driver (BPB, FAT chains, directory listing, read/write)
js/              a from-scratch JS engine: lexer, parser, tree-walking
                 interpreter, and the DOM bindings that connect it to net/dom.c
include/         public headers, mirroring kernel/, drivers/, gui/, net/, fs/, js/
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
Ethernet/ARP/IPv4/ICMP/UDP/DNS/TCP stack, a real read/write/create FAT32
filesystem, a web browser with a real (if pragmatic) CSS box-model
layout engine, clickable links, and file downloads, AC97 audio with
WAV playback, and a from-scratch JavaScript engine (lexer, parser,
tree-walking interpreter, and DOM bindings with onclick interactivity)
are now done (see above). Rough order of what's next:

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
6. **A CSS horizontal box model** — width/height, floats or flexbox,
   centering — the layout engine only stacks blocks vertically today.
7. **File delete/rename** — the FAT32 driver can create and overwrite
   files now, but there's still no way to remove or rename one from the
   File Manager.
8. **Growing the JS engine** — the interpreter is deliberately minimal
   today (integer-only numbers, no prototypes/classes, no `try`/`catch`,
   no external `<script src>` fetching). Next steps there would be a
   software fixed-point or soft-float number type (the kernel is built
   `-mno-sse -mno-80387`, so real floats need emulation, not just
   enabling the FPU), prototype-based objects, and wiring `<script src>`
   through the existing HTTP fetch code.

Each of these is independently a multi-day-to-multi-week task; happy to
keep building on any of them next.
