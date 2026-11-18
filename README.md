# Omi

A 64-bit operating system that boots on real hardware and runs programs in ring 3
with a real libc. Monolithic kernel, own drivers, own compositor, no Linux
underneath.

Forked from [vib-OS](https://github.com/viralcode/vib-OS) (a UEFI demo with a
window manager); what happened since is below. This tree is the whole OS: 46,846
lines of first-party kernel, 2,918 lines of user-mode programs, plus vendored
BearSSL and Gumbo.

![Omi desktop](screenshots/omi-desktop.png)

## What it does

**Boots.** UEFI and legacy BIOS from one hybrid ISO, on QEMU and from a USB stick
in a real machine. Limine hands over the framebuffer and a list of boot modules;
the kernel brings up its own GDT/IDT, paging and heap, then starts the first
process.

**Runs a desktop.** Compositor with double buffering, draggable and resizable
windows, a bottom taskbar and a magnification dock, wallpapers decoded from JPEG
at boot, midnight theme by default. Widgets: sticky note, big clock, weather.
Apps drawn by the kernel shell: calculator, notes, media player, file manager,
task manager (live process count). Omi Net, the browser, is not one of them: it
is a ring-3 process that opens a window and a surface and does its own fetching,
parsing (Gumbo) and layout.

**Talks to the network.** e1000 driver, ARP, IPv4, ICMP, UDP, TCP with a real state
machine and retransmission, a DNS resolver, and an HTTP client. The compositor
uses that stack to fetch and render remote images.

**Runs real programs.** An ELF loader with a 36-syscall native ABI, and enough of
the Linux ABI on top that static musl binaries work unmodified: `mmap`, `fork`,
`execve`, pipes, `epoll`, `memfd`, shared libraries, `clock_gettime`, futexes,
threads. Scheduling is 24 (`sched_yield`), so a spin with backoff actually lets
its peer run. BSD sockets (41/42/43/44/45/46/47/48/49/50/51/53/54/55 and 318) are implemented
so that a TLS stack can exist in user mode.

**Processes that talk to each other.** `socketpair(AF_UNIX, SOCK_STREAM)` gives two
descriptors wired to each other: full duplex, end-of-file when one end closes,
`EPIPE` rather than a hang when the other is gone, and a blocking `read` that is
woken by the far end writing - checked across two real processes, not in a loopback
shortcut. Two pipes do the work, which is why the semantics are the ones pipes
already had.

**A channel can be named, so the two sides need not exist at the same moment.**
`bind` claims a path, `listen` starts accepting, and `accept` returns a connection
as a new descriptor; on the other side `connect` takes a path and hands back the
client end. The client can be any process - it never had to be forked from the
server, and it never shares a descriptor with it. A name is a flat string, not a
file: sixteen names at once, sixty-four bytes each, a backlog of eight, no
directories and no permissions, because the filesystem has no directories to put
them in. The name exists while the listening socket is open and disappears when it
closes, so there is nothing to unlink and nothing to leave behind. Failures are
specific: `EADDRINUSE` for a name already claimed, `ENOENT` for a name nobody
bound, `ECONNREFUSED` for a name that was bound but never told to listen (or whose
backlog is full), `EINVAL` for `accept` on something that is not a listener, and
`EAFNOSUPPORT` for trying to bind an `AF_INET` address - there are no local ports
to claim, and reading a port number as a path would invent a name.

**A descriptor can be handed over that stream.** `sendmsg` with an `SCM_RIGHTS`
control message passes an open file description to the other end, which installs it
as a descriptor of its own; the sender can close its copy immediately and the
receiver's keeps working, because the description is reference counted rather than
copied - and it is the same open file description on both sides, so a shared offset
moves with it. The test that proves it is a child that never opened a pipe end,
writing into one it was given; pipes, sockets and memfds all travel this way, which
is the shape a browser's broker uses to hand a child a connection. A control buffer too small to hold the descriptor is
answered the way the ABI says - `MSG_CTRUNC`, and the descriptor released rather
than leaked - and an ancillary type this kernel does not implement is refused with
`EINVAL` instead of being quietly dropped. Two limits are real and stated rather
than hidden: the descriptors ride on at least one byte of data (a stream has no
notion of a message with no body to order them against), and eight such messages
can be in flight per direction before the send fails with `ENOBUFS`.

**TLS in user space, not the kernel.** `tlsget` links BearSSL against that socket
ABI and completes real handshakes with certificate-chain and hostname
verification; the kernel never sees a key.

**Windows whose pixels belong to the process.** A ring-3 window can map a surface,
write `0x00RRGGBB` words straight into it, and present a damage rectangle. The
kernel owns the frames and the placement; the compositor copies rows, with no
scaling and no resampling, so a surface pixel is a screen pixel. Eight surfaces
exist at once, 16 MB of address space each and up to 1920x1200, and frames are
allocated only for the pixels a surface actually has - a 440x360 window costs
634 KB, not a whole slot. Up to 64 ring-3 windows can be open.

**A 4 GB window per process, with real shared memory.** A program's address space
runs to 4 GB (page tables built on demand, so an empty window costs nothing),
which is the room a browser engine wants before it has drawn anything: V8's cage,
a code range, and a heap that only commits what it touches. Sixty-four named
shared regions of up to 16 MB each, refcounted and released when the last user
lets go, are how two processes hand a buffer to each other. Descriptors start at
16 and grow to 4096, because a browser opens hundreds - one per socket, pipe and
shared region.

**Audio.** HDA driver, and an MP3 decoder in the media player.

**A disk that outlives the boot.** An ATA PIO driver (IDENTIFY, then 28-bit LBA
reads and writes, every wait bounded so a missing drive is `-ENODEV` and not a
hang) with a small block-backed store on top: a superblock, 32 records of file
name and extent, files up to 64 KB, mounted into the same namespace as the boot
modules. A file written and closed, or followed by `sync()`, is on the sectors -
which is why a counter file walks 0, 1, 2 across three consecutive boots.

## What it doesn't do yet

- **The disk is a first step, not a filesystem.** One PIO drive on the primary
  channel, no partitions and no NVMe, no directories, 32 files of 64 KB each, and
  no free list - an extent is handed out by a bump allocator, so deleting a file
  frees the name and not the sectors until the disk is reformatted. There is no
  `fsck`: a torn write is detected by the superblock checksum and answered by
  reformatting, which loses the disk's contents rather than recovering them.
- **A name is not a file.** `bind`/`listen`/`accept`/`connect` work on `AF_UNIX`,
  so a stranger can find a channel by path, but the namespace is a table of
  strings: no `unlink` (a name dies with its listener), no permissions, no
  abstract namespace, no socket files in the filesystem, sixteen names at a time.
  `SOCK_DGRAM` pairs are refused rather than faked, and `SO_PASSCRED`/`ucred`
  (kernel-verified peer identity) does not exist, so a peer's pid cannot be
  trusted - a broker has to authenticate its clients by protocol, not by asking
  the kernel who they are.
- **One core.** No SMP: the kernel runs on the CPU the firmware started it on.
- **No GPU.** Every pixel is composed on the CPU.
- **The compositor redraws the whole scene.** Damage rectangles are tracked and
  counted for every presented surface, but the scene is rebuilt from the wallpaper
  each frame, so the row copy still covers the whole client rect rather than the
  rectangle that actually changed. Regions are what it has; clipping the frame to
  them is what it does not do yet.
- **Omi Net is a reader, not a browser.** A ring-3 process (`ominet.elf`) that
  the dock spawns; HTML and a little CSS, no JavaScript. Its text is one
  8x16 ASCII font: Latin-1 and the common typographic characters (`©`, `é`, `’`,
  `—`) are folded to their ASCII forms at layout time and survive, but anything
  outside that table - Greek, Cyrillic, CJK, emoji - is dropped rather than drawn
  as a blank box. Scrolling: arrow keys and Page Up/Down, Home/End, the scrollbar,
  and the wheel. A page body is capped at 256 KB - larger ones are parsed up to the
  cap and the status line says "first 256 KB only". That cap used to be 64 KB
  because a 128 KB page killed the parser: its allocator ran out of mmap slots,
  `malloc` returned NULL, and Gumbo wrote through it. The real fault was a 128-slot
  per-space area table, and it is now 512 - so a 130 KB page reads whole.

  Type: there is one face, an 8x16 bitmap, and a font id now means an integer
  scale of it rather than a wish. Headings (`h1`/`h2`, and `h3`/`h4`) are drawn at
  2x, body and code at 1x, and the measure function the client supplies multiplies
  by the same scale - which is the only reason a 2x heading wraps at the width it
  is actually drawn. A bullet is a 4x4 square before each list item; the face has
  no dot glyph and at this size the two shapes are the same. Form controls follow
  their block's alignment, so a field under a centred label is centred too.

  Clicking: text links, and pictures inside `<a href>` - a thumbnail is nearly
  always the link, and its rectangle is the biggest target on a gallery page, so
  image ops carry the anchor they sit in. Form fields take the click to focus and
  submit buttons submit. What is *not* clickable: an anchor whose only content is a
  styled empty block (nothing to hit-test), `<button>` (not laid out), `<select>`
  and `<textarea>`, anything that needs `onclick` (there is no scripting), and image
  maps. The pointer does not change over a link either - the cursor belongs to the
  compositor, not the page.

  Chrome: the address strip always shows the address. A failed load reports itself
  on a line of its own at the bottom of the window, because an address bar that
  turns into an error message hides where you are and reads as a dead window.
  A link with no scheme and no leading slash resolves against the page's own
  directory - treating `document` as a hostname asked DNS about a page name.

  Images: the first frame of a GIF is decoded and drawn, scaled to the box the
  page asked for. PNG, JPEG and SVG are not decoded at all - a page made of them
  shows its alt text, and the serial log says which file it could not read, because
  an empty rectangle and a broken decoder look the same to a reader. JPEG
  (baseline and progressive), PNG, BMP, TGA, PSD and PNM come from `stb_image`,
  which the kernel already carried for the media player, so the browser links the
  same code rather than growing a second decoder. What it costs is bounded by the
  machine, not by constants: the bitmap budget is a quarter of free memory as the
  kernel reports it (`SYS_MEMINFO`), the fetch buffer is sized from that budget,
  and image fetching stops after three seconds of wall clock - time being the real
  cost of an image here, at about a second of TLS each. A page with a hundred
  thumbnails used to fetch all hundred before its layout could finish.

  Forms work, as far as they go: `<input type="text">` is a real box you can click
  into and type in - backspace, Tab to the next field, Escape to leave it, Enter to
  submit - and `<input type="submit">` is a real button that submits by mouse. A
  GET form becomes a query string built from its fields, so a search box searches.
  What is missing is the rest of the family: `type="post"` is refused by name
  rather than sent as an empty POST, `<textarea>`, `<select>` and `<button>` are not
  laid out at all, and a field sits on its own line rather than beside its label,
  because the line buffer only carries text.
- **Drivers are device-specific.** Tested on QEMU's q35 and on one physical
  machine. A different NIC or audio codec is unsupported, not merely untested.

## Build

Needs clang/LLD, xorriso, and QEMU for testing. Limine 8.6.0 is downloaded on the
first build.

```bash
sudo apt install clang lld xorriso qemu-system-x86
make                 # -> omi.iso
```

## Run

```bash
make run             # QEMU, UEFI
make run-bios        # QEMU, legacy BIOS
```

By hand, with the serial log in a file - how it is tested here, with no display:

```bash
qemu-system-x86_64 -M q35 -m 512M -cdrom omi.iso -boot d \
    -display none -serial file:serial.log \
    -netdev user,id=n0 -device e1000,netdev=n0
```

Add a disk to it and the filesystem has somewhere to persist: the first boot
formats the blank image, the next one finds its files.

```bash
qemu-img create -f raw disk.img 64M
qemu-system-x86_64 -M pc -m 512M -cdrom omi.iso -boot d \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:serial.log
```

On a machine: write the ISO to a USB stick (`dd if=omi.iso of=/dev/sdX bs=4M`) and
boot it in UEFI or legacy mode.

## What proves it

Everything above is the output of a program that runs on the ISO at boot, not a
description of intent. A stock boot prints about 334 individual pass lines in the
serial log as `[tag] ok` - the exact number moves a little with what the network
answers, because the TLS and HTTP suites are part of it - and these numbered tallies:

| program | result | what it covers |
|---|---|---|
| `surftest` | 10/10 | surface mapping, present, damage, refusals |
| `omirender` | 12/12 | glyphs, wrap, clipping and blending in a ring-3 rasteriser |
| `disk` | 6/6 | with a disk attached: IDENTIFY, the counter across boots, a 6-sector file |
| `surfbig` | 10/10 | a 1920x1200 surface, shrink and grow, and the slot pool's `-ENOSPC` |
| `surfexit` | — | a window and its surface are reaped when the process exits |
| `vmtest` | 10/10 | the 4 GB window: 1.5 GB mapped, both ends real, 8 GB refused |
| `shmtest` | 12/12 | an 8 MB memfd aliased two ways, 16 regions live at once |
| `ominet` | 24/24 | name resolution, TLS, HTML parsing, layout, painting, scrolling |
| `tlsget` | 7/7 | real TLS handshakes from ring 3 |
| `socktest` | 20/20 | musl's `socket`/`connect`/`send` over the BSD ABI |
| `unixpair` | 11/11 | `socketpair` both ways, EOF, pool exhaustion, a read woken by another process |
| `unixname` | 11/11 | two forked clients find a bound path, get served, collect a reply; `EADDRINUSE`, `ENOENT`, `ECONNREFUSED`, `EINVAL`; closing the listener removes the name |
| `cred` | 7/7 | `SO_PEERCRED` in both directions, a forked descriptor still naming its maker, a listener and a TCP socket with no peer |
| `mem` | 2/2 | a process touches 64 MB of real pages, and the kernel reports its own free memory |
| `img` | 10/10 | an 8x8 GIF fixture decodes: size from the header, four palette quadrants in the right places, a truncated file refused |
| `form` | 16/16 | `<input>` becomes a named field belonging to a form: text, hidden and submit, action and method, boxes drawn, hidden not drawn |
| `scm` | 15/15 | `SCM_RIGHTS`: a pipe, a socket and a memfd received by a process that never opened them, `MSG_CTRUNC`, an unknown type refused |
| `linux3` | 27/27 | Linux syscall surface |
| `pipe` / `pipe3` | 37/37, 30/30 | pipes, blocking, EOF, teardown |
| `maptest` | 24/24 | file-backed `MAP_PRIVATE`, `MAP_FIXED`, `PROT_NONE` guards |
| `epolltest` | 24/24 | `epoll` add/mod/del/wait |
| `memfdtest` | 23/23 | `memfd_create` and sealing |
| `modtest` | 15/15 | loading a module from the boot medium |
| `dyntest` | 14/14 | shared libraries and the dynamic linker |
| `lives` | 8/8 | process lifetime and reaping |
| `exectest` | 7/7 | `execve`, arguments, descriptor survival |

The screen is checked from outside the OS, because a program cannot see its own
pixels: a QEMU `screendump` of the running system is parsed and the exact
coordinates compared (`surftest` and `omirender` print their geometry for this).
Its colour bars are four pixels wide on purpose - a blit shifted by one pixel, or
a downscale of one, does not survive that check.

The surface tests park in a blocking wait rather than exiting, so the dump is
taken with their windows alive; `ominet` fetches iana.org over TLS and paints
the page it received, and `surfexit` is the opposite test, filling a window
with a colour nothing else uses and returning from `main` immediately. Nothing of
that colour is left on screen afterwards, which is how the reaping is checked.

## Layout

```
kernel/
├── boot/        Limine entry, boot modules
├── drivers/     framebuffer, ps2, usb (ehci/xhci/hid), e1000, hda, acpi, pci
├── fs/          VFS, descriptors, pipes, sockets, epoll
├── gui/         compositor, shell apps, fonts, icons
├── mm/          paging, heap, copy-on-write
├── net/         TCP, DNS, HTTP
├── ring3.c      ELF loader, native syscalls, Linux ABI, surfaces
├── lib/         BearSSL, Gumbo, string
└── user/        the first process
userland/        ring-3 programs (musl-gcc): ominet (browser, and the only HTML
                 engine - omilayout.c), omirender, surftest, tlsget, socktest
limine.conf      bootloader config
Makefile         build
```

## Origin

Forked from `viralcode/vib-OS`, which provided the UEFI entry, framebuffer, JPEG
wallpapers and a first window manager. Everything above concerning processes,
networking, TLS, audio and surfaces is not from there.