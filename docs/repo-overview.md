# Omi, in one document

The whole repository: what is in it, what has changed, and how far it is from
running Chromium. `README.md` is the tour and `docs/chromium-roadmap.md` is the
external proposal this work is measured against; this file is the map that joins
them, and the status ledger that says which parts of that proposal are real.

Everything below is measured from this tree or from a boot log, not estimated.
Where something is not done, it says so.

## What Omi is

A hobby operating system for x86_64, written from scratch: its own kernel, its own
ring-3 userland, no Linux under it. It boots from a Limine ISO on bare metal and in
QEMU, brings up every core the firmware describes, runs a ring-3 compositor and
display server, has a TCP/IP stack and a TLS stack, and — since the work in this
document — runs V8 and Skia as ordinary userland processes.

It exists because Ryan wanted to know how far a hand-written OS could get toward
being a browser platform, and the answer is only credible if each step is proven
from outside the machine rather than asserted in a commit message.

The destination is Chromium. The near-term target is not.

## How a boot works

`limine.conf` names a kernel and a list of boot modules. The kernel is loaded at
`0xFFFFFFFF80000000`, the modules are handed over as blobs, and the kernel exposes
them as files under `/apps`, `/lib` and `/fonts` — there is no disk driver in the
boot path, so the ISO *is* the filesystem. Every boot module costs one slot in the
open-file table, which is why `RFILE_MAX` is a number that has to be watched.

Then `kernel/boot/limine_boot.c` spawns the test programs and applications it
wants, each as a ring-3 process with its own address space, and the kernel keeps
scheduling. A boot ends when the log says so; there is no init to exit to.

## The map

| Area | Lines | What it is |
| --- | --- | --- |
| `kernel/ring3.c` | 5,329 | The syscall surface: 59 Omi calls, plus the Linux ABI that musl, V8 and Skia actually use. Entry, argument marshalling, per-call validation. |
| `kernel/include` | 16,542 | Every structure and prototype. `task.h`, `mm.h`, `gui.h`, `fd.h`, `syscall.h`. |
| `kernel/gui` | 13,148 | Surfaces, windows, the compositor, input routing, the font path, scene handoff. |
| `kernel/drivers` | 6,300 | Framebuffer, PS/2 keyboard and mouse, ATA, PCI, timers, RTC. |
| `kernel/net` | 4,151 | Ethernet, ARP, IPv4, TCP with keep-alive, UDP, DNS, sockets. |
| `kernel/fs` | 3,785 | The module-backed filesystem, descriptors, paths, the RAM file area. |
| `kernel/mm` | 505 | Per-process address spaces, page tables, demand paging, COW. |
| `kernel/boot` | 844 | Limine handoff, MADT parsing, per-CPU bring-up. |
| `kernel/kernel_task.c` | 786 | Scheduler, task slots, threads, reclamation. |
| `kernel/elf.c` | 631 | ELF64 loader for static and dynamically linked executables. |
| the rest of `kernel/` | ~1,900 | `bt.c` backtraces, `work.c` multi-core tile work, `walltime.c`, `gdt.c`, `cow.c`, per-process user access. |
| `userland/` | 12,515 | 50 programs: the test suites, the shell, Omi Net, the compositor and display server, and the engine smoke tests. |
| `limine/` | 109,718 | Vendored bootloader. Third-party. |
| `kernel/lib` | 103,969 | Vendored BearSSL and Gumbo, plus `string.c` and a builtins shim. Third-party. |
| `kernel/media` | 101,196 | Vendored minimp3 and picojpeg, and the wallpapers as C arrays. |
| `docs/` | ~24,000 words | The design notes: `linux-abi.md`, `ring3-and-gui-ipc.md`, `readiness-design.md`, `chromium-roadmap.md`. |

Omi's own kernel code is about 53,500 lines. The rest is vendored.

## What runs, and what proves it

One boot of the current ISO produces **398 named `ok` checks across 34 suites, and
no `FAIL` lines**, plus per-suite tallies (`vfs 16/16`, `futex 3/3`). The suites are
not unit tests of functions — they are ring-3 programs that use the kernel the way
an application would:

- `maptest`, `memtest`, `vmtest`, `cowlives` — mapping, protection, demand paging,
  copy-on-write, address-space reuse.
- `fork`, `exec`, `pthreadtest`, `muslfork`, `muslthreads` — process and thread
  lifecycle, including musl's own pthreads.
- `sigtest` — 18 checks: dispositions, masks, pending, alternate stacks, delivery
  at safe points, `SIGSEGV` from a real protection violation.
- `vfs`, `modtest`, `dyntest`, `libmap`, `libprobe` — the filesystem,
  the module list, the dynamic linker.
- `epolltest`, `polltest`, `pipetest`, `eventfd`-shaped readiness, `memfdtest`,
  `shmtest`, `scmtest`, `socktest` — descriptor plumbing, including passing a
  socket between processes.
- `net`, `tcp`, `fetch`, `httpget` — the stack, up to a real HTTP fetch.
- `surftest`, `surfbig`, `surfexit`, `scene`, `comp` — surfaces, aliases, scene
  handoff, and the ring-3 display server.
- `sandtest` — syscall filtering.
- `cpptest` — 12/12, C++ with threads and exceptions.
- `v8hello` — V8 16.0.0 executes JavaScript, exits 0.
- `skiahello` — Skia rasterises, and the PNG it encodes has the same CRC on Omi as
  on Linux.
- `skiawin` — Skia draws into a window's own pixel buffer and the compositor puts it
  on the framebuffer, checked by reading the buffer back and by finding the marker
  colour at the expected screen coordinates in a framebuffer dump. It also draws text
  from the ISO's own faces, so the window on screen reads "Skia on Omi".
- `skiatext` — FreeType inside Skia, over `Bricolage.ttf` and `JBMono.ttf`. Prints the
  advance, the ink count and two CRCs that are identical to the same binary on Linux.
- `atxit`, `handoff` — the two regression tests that came out of the exit bug.

Two applications sit on top: **Omi Net** (fetching, HTML parsing via Gumbo, a real
CSS cascade with specificity and custom properties, layout, TrueType text) and
**the display server** (`comp.c`: window stacking, title bars, drag, close,
focus, damage-limited repaint — 3,001 patches and 12.6M pixels over a 3,000-frame
run against 3.07 billion if every frame were whole).

## What has changed

The repository is 135 commits. The arc, in the order it happened:

1. **A shell that draws.** Framebuffer, fonts, keyboard, mouse, a compositor in
   the kernel. Everything painted by the kernel.
2. **A real process model.** Per-process address spaces with a 4 GB window, ELF
   loading, `fork`, `execve`, demand paging, COW, threads and futexes.
3. **The kernel got out of the pixel business.** Surfaces became client-owned
   buffers: `SYS_SCENE_MAP`/`FLUSH`/`OWN`/`HANDOFF`, `SYS_SURF_MAP` for read-only
   aliases of any window, `SYS_SURF_LIST`, `SYS_WIN_GEOM`/`TITLE`/`PLACE`/`CLOSE`,
   and input handoff via `SYS_INPUT_TAKE`/`POLL`/`WIN_INPUT`. Then `comp.c` became
   a display server in ring 3 that composites, stacks, focuses and damage-tracks
   on its own, and the kernel stopped knowing what a window is.
4. **SMP.** Cores discovered from the MADT, one stack and one schedule each, 32
   maximum, verified at sixteen cores as well as four.
5. **Language and runtime readiness.** C++ with real threads and exceptions
   (`cpptest 12/12`), then signals — `rt_sigaction`, `rt_sigprocmask`, alternate
   stacks, `rt_sigreturn`, delivery at safe points, exception-to-signal mapping —
   which is the prerequisite V8's JIT traps need.
6. **The Linux ABI.** musl's own binaries run: `clone`, `futex`, `epoll`, `eventfd`,
   `pipe2`, `memfd_create`, `sendmsg`/`recvmsg` with `SCM_RIGHTS`, `poll`, `openat`,
   `fstat`, `getdents64`, `arch_prctl`, `mprotect`. `docs/linux-abi.md` is the
   ledger: which call, what it returns, and what it refuses.
7. **V8.** Built for `x86_64-linux-musl`, embedded in `v8hello.cpp`, executing
   JavaScript on Omi and exiting cleanly.
8. **Skia.** Built the same way, rasterising on Omi, deterministic against Linux.
9. **The two bugs that came out of 7 and 8**, both of which had been presenting as
   something else:
   - **A futex word is 32 bits; the kernel compared eight.** musl passes the
     expected value sign-extended, so a contended lock waits with
     `0xFFFFFFFF80000000`; read as a 64-bit word the compare never matched, every
     waiter got `-EAGAIN`, and musl retried forever with no syscall to show for it.
     The symptom was "`exit()` deadlocks".
   - **`RFILE_MAX` was 48.** Every boot module takes a slot; adding two test
     programs pushed `/lib/libgreet.so` off the end, and the symptom was `dyntest`
     reporting a missing symbol for a library plainly in the ISO. It is 112.
   - Also found on the way: `~DefaultPlatform` waits on the worker threads it just
     asked to stop, so returning from `main` blocks there. V8's own samples leak the
     platform; this embedder does too.

## The goal: Chromium

The roadmap in `docs/chromium-roadmap.md` is an external proposal (ChatGPT's, at
Ryan's request) with 100 steps in 8 stages. Its step numbers are its own; Omi's
`Roadmap step N` references are different and live in `docs/linux-abi.md`. What
follows is that proposal mapped onto what this repository actually contains.

| Stage | What it asks for | Where Omi is |
| --- | --- | --- |
| 1. Kernel and process foundations | Signals, `mprotect`, `madvise`, `prlimit64`, slot reclamation, exception handling | **Mostly done.** Signals (1–5), `mprotect` (6), slot reclamation (12) and thread lifecycle (13) are implemented and proven from the ISO. `munmap` partial unmapping (7), `madvise` (8) and `prlimit64` (11) are open; `SA_RESTART` restart semantics, stop/continue and `si_pid` are open. |
| 2. Linux syscall compatibility | `clone`, robust futexes, epoll, eventfd, `pipe2`, memfd, `SCM_RIGHTS`, `poll`, `openat`, `fstat`, `getdents64`, `prctl`, `arch_prctl` | **Largely done for what musl and V8 need.** `docs/linux-abi.md` lists 31 rows covering 55 Linux call numbers, each with the flags it honours and the cases where it answers `-ENOSYS` instead of pretending. `clone`, `futex`, `execve`, `mmap`/`mprotect`/`munmap`/`madvise`, timing, epoll, memfd, sockets and `arch_prctl` are all there. Gaps that matter: `set_robust_list` and `prctl` are absent, and Linux's `sendmsg` reports `-ENOSYS` for `msg_control` - descriptor passing works over Omi's own Unix sockets, not through the Linux ABI. Robust futexes become important the moment a process can die holding a lock. |
| 3. Runtime libraries | musl, compiler runtime, libstdc++, zlib, ICU, FreeType, HarfBuzz, Fontconfig, libpng, libjpeg, libwebp, libxml2, SQLite | **Partial, and further along than it was.** musl works (dynamic and static, threads, signals). C++ with exceptions and threads works. zlib, libpng, libjpeg-turbo and now **FreeType** (built into Skia, producing byte-identical text on Omi and Linux) are part of the Skia build. **Missing: ICU, HarfBuzz, Fontconfig, libwebp, libxml2, SQLite.** Omi's own kernel text is stb_truetype plus one 8×16 bitmap face; ring-3 apps can now go through FreeType instead. |
| 4. Graphics and window management | Surfaces, shared buffers, damage tracking, window metadata, focus, input, presentation, Ozone, Skia | **Strong except Ozone.** Surfaces (49–51), damage (53), focus and input (55–57) and presentation (59) are done and verified from screenshots. 54 is partial: a window is only discovered through its surface, so there is no window list independent of pixels. 61 is done: Skia draws straight into a window's buffer and the pixels are verified in a
framebuffer dump at exact coordinates, so the compositor path is real. **60 (Ozone backend) is not started.** |
| 5. V8 | GN build, cage, executable memory, signals, threads, GC, `d8` | **Started and green at the smoke-test level.** V8 builds for musl and runs JavaScript in a ring-3 process: cage reservation, JIT, GC, threads and signal-based traps all exercised. Not done: `d8` (70), WebAssembly, and the platform-teardown leak above. |
| 6. Blink and HTML rendering | Blink build, HTML/CSS parsing, layout, DOM bindings, Skia integration, fonts | **Not started, and the largest single item.** Omi Net has a hand-written parser, cascade and box model; that is a test application, not Blink. |
| 7. Chromium infrastructure | `//base`, Mojo, process launcher, network service, BoringSSL, storage, Content Shell | **Not started.** The prerequisites exist: Unix sockets with descriptor passing, shared memory, `fork`/`execve`, a sandbox filter, and BearSSL for TLS. |
| 8. First functional browser | Navigation, HTTP/HTTPS, JS, input, profiles, sandboxing | **Not started.** Omi Net already loads real HTTP pages and renders them, which is the shape of the acceptance test without the engine. |

## The order of work from here

1. ~~**Put Skia on the screen.**~~ **Done.** `skiawin` wraps a window's pixel buffer
   in an `SkSurface`, and the framebuffer dump shows its output at the predicted
   coordinates. Two API traps came out of it: `SYS_WIN_RECT` packs x and y into one
   argument and takes the size in the fourth (which travels in `r8`, not `r10`), and a
   window's outer height includes the title bar, so the surface was being clipped until
   the window was opened 31 rows taller.
2. **A real font stack.** ~~FreeType~~ **done** — it is inside Skia and renders text that
   hashes the same as Linux. What remains is HarfBuzz for shaping and ICU for breaking and
   bidi, plus a matching story so a page can ask for a family and get a face.
   into the display path. A browser without text is not a browser, and this is also
   Stage 3's biggest gap.
3. **Finish Stage 1.** `munmap` partial unmapping, `madvise`, `prlimit64`,
   `SA_RESTART`, stop/continue. These are small and they are what a runtime expects.
4. **`d8` on Omi.** V8's own shell, so the engine is tested by V8's tests rather
   than by one smoke program.
5. **Mojo's prerequisites, then Mojo.** Shared memory plus descriptor passing is
   the transport; the honest way to find out what else Mojo needs is to build it.
6. **Then, and only then, Blink.** It is the largest item in the plan and every
   week spent on it before the platform is stable is a week spent debugging two
   things at once.

## What this is not

- Not a Linux distribution, and not trying to be. The Linux ABI exists because
  musl, V8 and Chromium speak it, not as a goal in itself.
- Not a desktop. The display server is there to prove the compositor model.
- Not GPU-accelerated. Every pixel is CPU-composited, and that is fine until
  something is measured to be too slow.
- Not finished. The distance to Chromium is real: Blink, Mojo, ICU, HarfBuzz and
  the storage layer are each substantial, and the estimate that matters is not a
  number of steps but the next one.
