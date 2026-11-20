# A Chromium roadmap for Omi

An external proposal, pasted in whole on 2026-11-19 (written by ChatGPT, at Ryan's
request), kept here because it is a useful shape for the work even where it is
wrong about Omi's starting point.

**Read it as a proposal, not as Omi's own roadmap.** Two things to keep straight:

- Its step numbers 1-100 are *its own*. They are not the `Roadmap step N`
  references in `docs/` (see `docs/linux-abi.md`), and not the six-item "Missing,
  in the order it blocks progress" list in the README. Nothing here renumbers
  those.
- Stage 1 and Stage 2 assume a kernel that has almost nothing. Omi already has
  much of Stage 2 (epoll, futex, pipes, eventfd-shaped readiness, Unix sockets
  with `SCM_RIGHTS`, shared memory) and most of Stage 4 (a ring-3 compositor,
  surfaces, window metadata, focus, keyboard and pointer routing, damage-limited
  flush). What it does *not* have is Stage 1 - signals, `mprotect`, `madvise`,
  robust futexes, `prlimit`, reliable task-slot reclamation - and that is the
  part V8 cannot start without.

---

Target: run a minimally functional Chromium browser natively on Omi x86_64.

The roadmap is organized as a dependency chain. Each step introduces a specific
kernel feature, syscall, library, driver, or userspace capability required by
subsequent steps.

The initial goal is not GPU acceleration, a complete Chrome product, or a
polished desktop. It is getting Chromium Content Shell to launch, render
HTML/CSS, execute JavaScript, accept input, and load HTTPS websites.

Implementation work is separated from verification: a syscall is not complete
merely because it exists, it must behave correctly under realistic workloads.

## Stage 1: Kernel and process foundations

Objective: make Omi capable of reliably running complicated multithreaded
applications with independent address spaces, protected memory, and recoverable
exceptions.

| Step | Implementation | Detailed requirements |
| --- | --- | --- |
| 1 | `sigaction` / `rt_sigaction` | Per-process and per-thread signal dispositions. `SIG_DFL`, `SIG_IGN`, custom handlers. `SA_SIGINFO`, `SA_RESTART`, `SA_ONSTACK`, `SA_NODEFER`. |
| 2 | `sigprocmask` / `rt_sigprocmask` | Per-thread signal masks. Blocking, unblocking, replacing. Distinguish blocked, pending, ignored. |
| 3 | Signal delivery | Deliver at safe points during execution. Synchronous exceptions: `SIGSEGV`, `SIGILL`, `SIGBUS`, `SIGFPE`. Preserve interrupted register state. |
| 4 | `sigaltstack` | Alternate signal stacks per thread. Validate stack addresses and sizes. `SS_DISABLE`, `SS_ONSTACK`. |
| 5 | `rt_sigreturn` | Restore registers, instruction pointer, stack pointer, signal mask, flags from the signal frame. Validate the frame before returning to userspace. |
| 6 | `mprotect` | Change page permissions between readable, writable, executable. Enforce NX and write protection. Appropriate TLB invalidation. |
| 7 | `munmap` improvements | Partial unmapping, adjacent mapping merging, page-table cleanup, safe reclamation of physical pages. |
| 8 | `madvise` | Useful advice operations, especially `MADV_DONTNEED`, `MADV_FREE`, `MADV_WILLNEED`. |
| 9 | `getrandom` | Cryptographically secure random bytes. Entropy sources initialized at boot. Handle requests larger than one internal buffer. |
| 10 | `clock_gettime` | `CLOCK_REALTIME`, `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW` where appropriate. Nanosecond resolution, consistent per-CPU timekeeping. |
| 11 | `prlimit64` | Resource limit retrieval and modification: address space, stack, file descriptors, processes. |
| 12 | Task slot reclamation | Reclaim terminated process and thread slots. Release kernel stacks, page tables, descriptors. Support repeated creation and destruction without exhausting task IDs. |
| 13 | Thread lifecycle | Reliable thread creation, exit, joining, cleanup. Thread-local storage and per-thread kernel state destroyed correctly. |
| 14 | Exception handling | Reliable page faults, general protection faults, invalid opcode, floating-point exceptions. Distinguish kernel faults from recoverable userspace faults. |

Status (2026-11-19): steps 1-6 are implemented and proven from the ISO, not from
host tests. `userland/sigtest.c` boots as a boot module and reports 18/18:

- `rt_sigaction`: handler, `SIG_DFL`, `SIG_IGN`, `SA_SIGINFO`, `SA_ONSTACK`,
  `SA_NODEFER`, the handler's own mask, and the old-action return. Dispositions
  live in the address space, so they survive `fork` the way POSIX says they do;
  `SIGKILL` and `SIGSTOP` are refused with `-EINVAL` and bit 8 of the mask is
  re-ORed in on every check, so they cannot be caught or blocked.
- `rt_sigprocmask` / `rt_sigpending`: per-thread mask, block/unblock/replace, and
  the blocked-vs-pending distinction (a signal that is raised while blocked is
  delivered the moment the mask is lowered, and reads as pending until then).
- Delivery happens at two safe points: syscall return, and the fault entry. Both
  `kill`/`tkill`/`tgkill` and the synchronous `#PF`/`#GP` doors go through the
  same frame builder, so `si_addr` and `uc_mcontext.gregs[REG_RIP]` are what a
  JIT expects, and a handler that rewrites `RIP` resumes instead of dying.
- The frame is musl's `ucontext_t` layout (`mcontext` at 40, `uc_sigmask` at 296,
  `ucontext_t` at 936, `siginfo_t` after it, 1072 total). Anything else silently
  corrupts every `SA_SIGINFO` handler that reads `si_addr`.
- `sigaltstack`, including `SS_ONSTACK`/`SS_DISABLE` and inheritance across
  `fork`. Proven by catching a real stack overflow, which is only catchable
  because the handler runs on the alternate stack.
- Death by signal is distinguishable from exit: the kernel encodes it as
  `0x100 | sig` and `wait4` reports `WIFSIGNALED`/`WTERMSIG`, which is what an
  uncaught `SIGSEGV` and `SIG_DFL` termination both need.
- `mprotect` write protection is enforced and reaches a handler (the `maptest`
  reader/writer cases now assert `WIFSIGNALED` + `SIGSEGV`).
- `SIGCHLD` is raised when a child process exits (a thread exit is not, since it
  shares the address space). Without it a process host has to poll `wait`, which
  is exactly what Chromium does not do.
- Which signal an exception becomes follows the ISA: `#DE` and `#MF` are
  `SIGFPE`, `#UD` is `SIGILL`, `#AC` is `SIGBUS`, `#PF`/`#GP` are `SIGSEGV`.
  Only `#PF` may be served as demand paging or COW - a `#DE` carries no address,
  so `cr2` still holds the previous page fault's, and serving that maps an
  unrelated page and faults forever. A divide error cannot be stepped over
  either, so the test leaves the handler with `siglongjmp`, which is what a
  runtime does with an exception it cannot retry.

Still open in Stage 1: `SA_RESTART`/`EINTR` restart semantics for blocking
syscalls (`pause` returns `-EINTR`, the rest return partial work rather than
restarting); stop/continue (`SIGSTOP`/`SIGTSTP`/`SIGCONT` are treated as ignore,
there is no stopped task state); `si_pid`/`si_uid` in `si_code == SI_USER`
(handlers get the right signo and address, but not who sent it); the `#AC` gate
exists but nothing sets `EFLAGS.AC` yet, so `SIGBUS` from an alignment check is
untested. Then steps 7, 8, 11, 12 and 13 (`munmap` partial unmapping, `madvise`,
`prlimit64`, task slot reclamation, thread lifecycle). Steps 9 and 10 already
existed.

Two unrelated suites fail on every boot, before and after this work: `vfs`
"write then read back through a new descriptor" and `stat` "open: No space left
on device". They are not signal work and were already failing in the baseline.

Acceptance tests:

- A process deliberately triggers a segmentation fault; its handler runs and returns.
- A process uses an alternate signal stack to handle stack overflow.
- A process changes a page to read-only and receives `SIGSEGV` when writing to it.
- A process creates and destroys thousands of threads without exhausting task slots.
- Memory mappings can be created, modified, partially unmapped, and recreated.
- Faults in one process do not crash the kernel or unrelated processes.

Milestone: Omi has a dependable process and memory environment capable of
supporting V8's execution requirements.

## Stage 2: Linux syscall compatibility

Objective: expand Omi's Linux ABI until it can support the Linux userspace
interfaces required by Chromium and its dependencies. Prefer Linux compatibility
over inventing Omi-specific equivalents for every browser requirement.

| Step | Implementation | Detailed requirements |
| --- | --- | --- |
| 15 | `clone` | Process and thread creation flags: `CLONE_VM`, `CLONE_FS`, `CLONE_FILES`, `CLONE_SIGHAND`, `CLONE_THREAD`, TLS setup. |
| 16 | `set_tid_address` | Clear-child-tid behavior. Write zero to the registered address on thread exit and wake appropriate futex waiters. |
| 17 | `set_robust_list` | Per-thread robust futex lists, processed when threads terminate unexpectedly. |
| 18 | `futex` improvements | `FUTEX_WAIT`, `FUTEX_WAKE`, bitset operations, requeue, private futexes. Correct timeout and interruption handling. |
| 19 | `epoll_create1` | Independent epoll instances, correct descriptor lifecycle, close-on-exec. |
| 20 | `epoll_ctl` | Add, modify, remove monitored descriptors. Level-triggered and edge-triggered behavior. |
| 21 | `epoll_wait` / `epoll_pwait` | Return ready descriptors, support timeouts, handle interrupted waits and signal masks. |
| 22 | `eventfd2` | 64-bit event counters, blocking and nonblocking reads, writes, polling integration. |
| 23 | `pipe2` | Anonymous pipes, independent read/write ends, blocking behavior, EOF, `O_CLOEXEC`. |
| 24 | `memfd_create` | Anonymous memory-backed descriptors supporting mapping, resizing, sharing between processes, sealing where required. |
| 25 | `fcntl` | Descriptor duplication, `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`, file locking where needed. |
| 26 | `sendmsg` / `recvmsg` | `SCM_RIGHTS` descriptor passing, ancillary data, scatter/gather buffers, nonblocking operation. |
| 27 | `poll` / `ppoll` | Readiness monitoring for sockets, pipes, eventfds and other supported descriptors. |
| 28 | `openat` | Directory-relative file access, flags, permissions, path resolution. |
| 29 | `fstat` / `newfstatat` | Consistent metadata: inode, size, mode, timestamps, file type. |
| 30 | `getdents64` | Directory enumeration with Linux-compatible directory entry structures. |
| 31 | `readv` / `writev` | Scatter/gather I/O for files, pipes, sockets. |
| 32 | `dup3` | Descriptor duplication with atomic close-on-exec. |
| 33 | `prctl` | The specific process configuration options needed by Chromium and its dependencies. |
| 34 | `arch_prctl` | x86_64 thread-local storage: `ARCH_SET_FS`, `ARCH_GET_FS`. |

Acceptance tests:

- Compile a Linux-targeted multithreaded test application against musl.
- Run multiple threads sharing memory and synchronizing through futexes.
- Pass file descriptors between separate processes.
- Monitor hundreds of simultaneous descriptors through epoll.
- Create shared memory with `memfd_create` and map it into multiple processes.
- Verify correct behavior under process termination and descriptor cleanup.

Milestone: Omi can support substantially more complex Linux userspace
applications without application-specific kernel modifications.

## Stage 3: Runtime libraries

Objective: establish the userspace library environment needed to compile and run
Chromium's dependencies. Libraries in dependency order, with working
cross-compilation and runtime tests.

| Step | Library | Implementation details |
| --- | --- | --- |
| 35 | musl | Missing libc functionality, thread support, signal interfaces, memory mapping, dynamic linking, syscall wrappers. |
| 36 | Compiler runtime | compiler-rt or appropriate GCC runtime components: integer operations, atomics, compiler-generated helpers. |
| 37 | libstdc++ | C++ standard library: threading, exceptions, RTTI, synchronization. |
| 38 | zlib | Compression and decompression required by Chromium dependencies. |
| 39 | libffi | Runtime calling conventions and foreign-function interfaces where dependencies require them. |
| 40 | ICU | Unicode handling, locale data, character conversion, normalization, internationalization. |
| 41 | FreeType | Load TrueType and OpenType fonts, rasterize glyphs, font metrics and hinting. |
| 42 | HarfBuzz | Unicode text shaping, glyph substitution, ligatures, bidirectional text. |
| 43 | Fontconfig | Font discovery, family matching, aliases, fallback fonts, configuration. |
| 44 | libpng | PNG decoding and the interfaces graphics components require. |
| 45 | libjpeg-turbo | JPEG decoding using its optimized implementation where supported. |
| 46 | libwebp | WebP decoding. |
| 47 | libxml2 | XML parsing for components that require it. |
| 48 | SQLite | Embedded database support for browser storage components where required. |

Additional build infrastructure:

- A reproducible x86_64 cross-compilation toolchain.
- A standardized Omi sysroot.
- Dependency version pinning.
- Static and dynamic linking where necessary.
- Library installation and loading conventions.
- Compatible exceptions, TLS and C++ ABI behavior across libraries.

Acceptance tests: compile a small C and C++ application using each library and
run it on Omi, rather than merely checking that the library builds.

Milestone: Omi has a reusable userspace development environment for complex C and
C++ applications.

## Stage 4: Graphics and window management

Objective: provide the display functionality Chromium needs without implementing
an entire X11 or Wayland stack.

| Step | Implementation | Detailed requirements |
| --- | --- | --- |
| 49 | Surface lifecycle | Create, register, destroy, query compositor surfaces. |
| 50 | Buffer resizing | Dynamically resize shared surfaces when windows change dimensions. |
| 51 | Shared-memory buffers | Efficient buffer exchange between clients and the compositor. |
| 52 | Buffer synchronization | Clients cannot modify buffers while the compositor reads them. Synchronization and ownership rules. |
| 53 | Damage tracking | Track changed regions, repaint only damaged areas. |
| 54 | Window metadata | Position, dimensions, visibility, stacking order, title, window state, independent of surface availability. |
| 55 | Window focus | Focus acquisition, focus loss, activation, input ownership. |
| 56 | Keyboard input | Key press/release events, modifiers, key mapping, focus-aware routing. |
| 57 | Pointer input | Movement, buttons, wheel events, pointer position. |
| 58 | Window lifecycle | Minimize, maximize, resize, close, application disconnect handling. |
| 59 | Display presentation | Present compositor buffers to the framebuffer without tearing or stale content. |
| 60 | Ozone platform backend | Chromium's Ozone interfaces: platform init, window creation, input, display discovery, buffer presentation. |
| 61 | Skia | Build Skia with a CPU rasterization backend and integrate it with Omi's display surfaces. |

Ozone is Chromium's platform abstraction: the layer to connect Chromium to Omi's
compositor, rather than implementing an entire Linux desktop display protocol.

Acceptance tests:

- Create several independent windows.
- Resize them while continuously rendering.
- Route input to the correct focused window.
- Destroy windows while rendering is active.
- Render Skia graphics into an Omi window.
- Verify no shared-buffer corruption during concurrent rendering.

Milestone: a standalone Skia application can draw and update graphics inside an
Omi desktop window.

## Stage 5: V8 JavaScript engine

Objective: get Chromium's JavaScript engine running independently before
integrating the whole browser. One of the most important milestones.

| Step | Implementation | Detailed requirements |
| --- | --- | --- |
| 62 | V8 build integration | Configure V8's GN build for x86_64 and Omi's compiler/toolchain. |
| 63 | Virtual memory integration | V8's address-space reservation, allocation, protection, release. |
| 64 | Pointer compression cage | The large, aligned virtual-memory reservation V8's pointer compression requires. |
| 65 | Executable memory | Executable memory allocation, JIT code generation, permission transitions. |
| 66 | V8 signal integration | Connect V8's expected fault handling and signal mechanisms to Omi's signals. |
| 67 | Thread integration | V8 can create and manage background threads and synchronize them. |
| 68 | Garbage collection | Allocation, marking, sweeping, compaction, GC under memory pressure. |
| 69 | Code range management | Reservation and management of executable code ranges. |
| 70 | V8 runtime testing | Run JavaScript through V8's standalone `d8` shell. |

Test specifically: objects, arrays, functions and closures; promises and
asynchronous execution; large heap allocations; GC under memory pressure;
multiple execution contexts; JIT compilation; WebAssembly as a later extension.

Acceptance test: build `d8`, boot Omi, and execute a JavaScript program that
performs substantial allocations, creates functions, runs asynchronous
operations, and survives multiple garbage collection cycles.

Major milestone: standalone JavaScript works on Omi.

## Stage 6: Blink and HTML rendering

Objective: establish the actual browser rendering engine.

| Step | Implementation | Detailed requirements |
| --- | --- | --- |
| 71 | Blink build integration | Configure Blink and its required Chromium dependencies. |
| 72 | HTML parser | Tokenization, parsing, DOM construction, error recovery. |
| 73 | CSS parser | Tokenization, selectors, cascading, inheritance, computed styles. |
| 74 | Layout engine | Block, inline, flexbox, and eventually grid layout. |
| 75 | DOM bindings | Connect DOM objects and browser interfaces to V8. |
| 76 | Event system | DOM events, event propagation, input event delivery. |
| 77 | Skia integration | Connect Blink's paint output to Skia. |
| 78 | Image decoding | Connect PNG, JPEG, WebP and other required decoders. |
| 79 | Font integration | Connect FreeType, HarfBuzz, Fontconfig to browser text rendering. |
| 80 | JavaScript execution | Execute scripts within the page's V8 context. |

Acceptance test: render an HTML document containing external CSS, JavaScript,
images, multiple fonts, flexbox layout, interactive buttons and DOM
manipulation, correctly inside an Omi compositor window.

## Stage 7: Chromium infrastructure

Objective: integrate the browser's multiprocess architecture and supporting
services.

| Step | Implementation | Detailed requirements |
| --- | --- | --- |
| 81 | Chromium base | Core utilities, task scheduling, threading, memory utilities, platform abstractions. |
| 82 | Mojo IPC | IPC transport, message pipes, handles, shared buffers, cross-process communication. |
| 83 | Process launcher | Browser, renderer and utility process creation through Omi's Linux ABI. |
| 84 | Network service | Socket handling, connection management, request scheduling. |
| 85 | BoringSSL | Chromium's TLS implementation, connected to Omi's socket infrastructure. |
| 86 | URL library | URL parsing, normalization, canonicalization, origin handling. |
| 87 | Storage services | Filesystem and database interfaces for browser profiles and persistent data. |
| 88 | Ozone integration | Chromium's windowing and display initialization onto the Omi Ozone backend. |
| 89 | Content Shell | Configure and build Chromium Content Shell without the full Chrome product interface. |

Chromium's multiprocess architecture uses Mojo extensively. Omi's Unix sockets
and descriptor passing are useful foundations, but Mojo's actual transport and
shared-memory requirements still need to be verified.

Acceptance test: Content Shell launches and creates separate browser and renderer
processes; the renderer talks to the browser through Mojo; the browser creates a
visible Omi window.

## Stage 8: First functional Chromium browser

Objective: get a real website working, not just a Chromium window.

| Step | Implementation | Detailed requirements |
| --- | --- | --- |
| 90 | Browser initialization | Start the browser process, initialize core services. |
| 91 | Renderer initialization | Launch renderers, establish communication with the browser. |
| 92 | Navigation | Page navigation and document loading. |
| 93 | HTTP | Load external sites over HTTP. |
| 94 | HTTPS | Load external sites over HTTPS with certificate verification. |
| 95 | JavaScript | Execute scripts, handle asynchronous page operations. |
| 96 | Input | Route keyboard, pointer and scrolling events to the correct page. |
| 97 | Persistent profiles | Cookies, local storage, preferences, cache. |
| 98 | Navigation controls | Address bar, back, forward, reload. |
| 99 | Process sandboxing | Restrict renderers: syscall filtering, filesystem restrictions, descriptor permissions. |
| 100 | Stability testing | Repeated navigation, multiple tabs, process crashes, memory pressure, recovery. |

Final milestone, Chromium running on Omi. The browser can: launch from the Omi
desktop; open a website; render HTML and CSS; execute JavaScript; display images
and fonts; load HTTPS sites; accept keyboard and mouse input; create and destroy
renderer processes; save basic browser data. GPU acceleration, audio, video,
extensions and advanced browser functionality remain future work.

## Recommended development priorities

Not every step is equally important. These are the milestones that avoid
spending time on features that do not unblock Chromium.

| Priority | Milestone | Why it matters |
| --- | --- | --- |
| 1 | Signal handling and memory protection | V8 prerequisite |
| 2 | Linux threading and IPC | Chromium process architecture |
| 3 | Complete compositor and Ozone | Actual browser window |
| 4 | Runtime libraries | Chromium build dependencies |
| 5 | Standalone V8 | JavaScript execution |
| 6 | Skia and Blink | Actual web rendering |
| 7 | Mojo and process launcher | Browser multiprocess operation |
| 8 | Content Shell | First Chromium application |
| 9 | Networking and storage integration | Functional websites |
| 10 | Sandboxing and stability | Safe, reliable browser |

## Corrections to the obvious reading of this plan

- Do not port every Linux syscall before attempting V8. Implement the known
  requirements, then use actual build and runtime failures to find the missing
  interfaces.
- Do not port all of Chromium's dependencies blindly. Use the pinned Chromium
  revision's GN dependency graph to determine which libraries are actually
  required; some features can be disabled for the initial build.
- Do not implement GPU support yet. Software rendering is enough to prove the
  architecture.
- Do not make Omi Net the permanent browser engine. Keep it as a test application
  and networking demonstration while Blink becomes the rendering engine.
- Do not postpone storage indefinitely. A minimal directory-based filesystem and
  persistent storage interface should exist before browser profiles and cache
  integration.

## Three build targets

Keep the project organized around three targets, so a Chromium failure does not
obscure whether the kernel, V8 or the graphics infrastructure is at fault.

| Target | What it proves |
| --- | --- |
| `omi-native-tests` | Kernel and native userspace functionality |
| `omi-v8-tests` | JavaScript engine compatibility |
| `omi-chromium` | Actual Chromium integration |

The central objective is to make each milestone independently testable,
reproducible, and useful to the next one, so every feature has an identifiable
reason for existing.
