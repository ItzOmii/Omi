# The Linux ABI, as far as it goes

Roadmap step 5. A second door into the kernel - the `syscall` instruction - that
carries Linux x86-64 numbers and lands on the same handlers `int $0x80` uses.

## Two number spaces, one kernel

`int $0x80` carries Omi's own small numbers; `syscall` carries Linux's. They are
deliberately separate tables, because a number alone means nothing: 34 is `pipe`
through one door and `pause` through the other. The entry that delivered the
number decides which table reads it, and `linuxtest` checks exactly that case.

That also means a program cannot accidentally straddle the two. A libc either
uses `syscall` throughout or it does not work at all - which is the failure mode
worth having, since a half-translated program would corrupt state quietly.

## The entry stub

`syscall` does not touch `rsp`. The user stack is still live when control arrives
at `LSTAR`, so the first two instructions have to be a stack switch, and doing it
without a scratch register means two rip-relative accesses:

```
movq %rsp, r3_user_rsp(%rip)
movq gdt_syscall_rsp(%rip), %rsp
```

`IA32_SFMASK` clears `IF` on entry, and the kernel never preempts CPL 0, so
nothing can run between those two instructions on a uniprocessor. `r3_user_rsp`
is one global rather than per-CPU because there is one CPU, and re-entering
`syscall` while inside the stub is not possible with interrupts off.

`gdt_syscall_rsp` is written by `gdt_set_kernel_stack`, the same function that
maintains `tss.rsp0`. Two variables, one writer: they cannot drift apart, and
every task switch already calls it.

After the switch, the stub rebuilds the frame `int $0x80` would have left - ss,
rsp, rflags, cs, rip - and falls into a shared dispatch tail. One return path
(`iretq`, pops, `fxrstor`) serves both doors, so there is no second place for the
return convention to be wrong.

`rcx` and `r11` are already destroyed by the instruction itself: `SYSCALL` puts
the return rip in `rcx` and the flags in `r11`. Saving them costs nothing and the
ABI does not promise them back.

**No `swapgs`.** There is no per-CPU GS base yet, so `syscall` has nothing to
swap to. Adding it belongs with SMP, not before.

## What is implemented

| Linux | name | notes |
|---|---|---|
| 0,1 | read, write | the existing handlers |
| 2,3,8 | open, close, lseek | `open`'s mode argument ignored |
| 9,11 | mmap, munmap | anonymous, or file-backed `MAP_PRIVATE`; `MAP_FIXED` replaces exactly the range it names, splitting what it lands inside; `MAP_SHARED` over a file is `-ENOSYS` |
| 10 | mprotect | per-area `PROT_*` bits plus live PTEs; `PROT_NONE` releases the pages, and a range spanning several areas takes each area's intersection |
| 12 | brk | one lazily-populated 16 MB area, pointer moved inside it |
| 16 | ioctl | `TCGETS`/`TIOCGWINSZ` only |
| 20 | writev | loops `write`; a partial write is a success |
| 22 | pipe | the step-4 pipe |
| 35,230 | nanosleep, clock_nanosleep | a deadline in ticks; a request shorter than one tick still yields; `TIMER_ABSTIME` against `CLOCK_REALTIME`/`CLOCK_MONOTONIC`; no `-EINTR`, because nothing delivers a signal |
| 39,186,218 | getpid, gettid, set_tid_address | the task id |
| 56,57 | clone, fork | `fork` ignores the flags it cannot honour; a thread shares the table |
| 59 | execve | replaces image, stack, areas and CR3; descriptors and pid stay; `envp` ignored; refused when the space is shared with a thread |
| 61 | wait4 | `WNOHANG` honoured, `rusage` zeroed |
| 60,231 | exit, exit_group | one exit path shared with SYS_EXIT |
| 96,228 | gettimeofday, clock_gettime | the tick counter |
| 102..110 | getuid/getgid/euid/egid/getppid | zero |
| 158 | arch_prctl | `ARCH_SET_FS` / `ARCH_GET_FS` |
| 202 | futex | Linux op numbers, timespec converted to ticks |
| 232,233,291 | epoll_wait, epoll_ctl, epoll_create1 | a kept set of descriptors, reported level-triggered; `EPOLLONESHOT` removes the entry after it is reported; `EPOLLET` is `-EINVAL`; `EPOLL_CLOEXEC` accepted and ignored |

Everything else returns `-ENOSYS` and is logged once per number. Logging once
matters: a libc probing for a syscall in a loop would otherwise fill the serial
log and hide the line that explained the failure.

## Three lies, each said out loud

**`ioctl(TCGETS)` claims stdout is a terminal.** There is no line discipline. The
flags describe intent (canonical, echo, signals) so musl line-buffers instead of
block-buffering until exit. Answering `-ENOTTY` honestly would be the wrong lie.

**`clock_gettime(CLOCK_REALTIME)` returns time since boot.** There is one clock:
the PIT tick counter at 100 Hz. Intervals are right; wall-clock dates are wrong by
however long the machine has been up. There is no RTC driver yet.

**`arch_prctl(SET_FS)` writes a per-CPU MSR.** `IA32_FS_BASE` is not per-task, so
a second thread with its own TLS block would clobber the first. Correct for one
thread, which is every program that exists today; per-task save/restore belongs
with the rest of the `%fs` gap in step 3.

## The low half belongs to the program

The first real musl binary failed on its very first instruction: a protection
fault at its own entry point, `err=0x15`. Nothing was missing - 0x400000 was
already mapped, by the kernel, supervisor-only. `ring3_new_space` copies all 512
PML4 entries from the kernel and then clears slot 1, so a user address space
inherits the kernel's identity map in slot 0. Every low address belongs to
somebody else.

Three ways out, and only one of them was honest:

- **Link musl high**, at our window. Impossible: musl's startup uses 32-bit
  absolute relocations (`R_X86_64_32`), which cannot reach 0x8000000000, and
  `-mcmodel=large` does not fix the crt objects that carry them.
- **Pick a low base that happens to be free.** Fragile by construction - it
  depends on the kernel image size and on how much RAM is installed, so it works
  in QEMU with 512 MB and breaks somewhere else.
- **Give the program its own tables there.** Done: `walk_user` now handles PML4
  slot 0 as well, and the first low mapping copies the PDPT into a frame this
  address space owns. The kernel's own tables are never written. A page
  directory is copied the same way the first time a 1 GB region is touched, and
  a 2 MB identity page is replaced by a table in that copy - the identity map's
  pages are built from 2 MB entries, so a program always lands on one.

Ownership is one software bit, `PTE_OWNED` (bit 10) on table entries, ignored by
the MMU. `mm_free_tables` walks both halves and frees only what carries it:
inherited tables point into the kernel's page tables, and freeing those would
take the machine down on the next task switch.

**What this gives up:** within an address space that runs a low program, the 2 MB
region the program occupies is no longer the kernel's identity map. The kernel
reaches physical memory through the HHDM everywhere (`hhdm_offset + phys`), not
through the identity map, which is what makes the trade safe - but it is a real
trade, not a free one.

## A real libc

`userland/muslhello.c`, built with `musl-gcc -static -O2 -no-pie` (musl-tools
1.2.4), loaded at its natural 0x400000 from a Linux-shaped stack:

```
[musl] hello from a real libc (argc=2)
[musl] argv[0] = muslhello
[musl] argv[1] = --from-omi
[musl] malloc works, pid=29, sizeof(long)=8
[musl] monotonic clock: 4.850000000
[musl] 1 MiB touched, byte[1048575]=Z
[musl] OK
[ring3] task muslhello exited with code 0x0
```

printf and malloc are the real implementations, not a shim: musl's stdio laid
out its buffers with `mmap`, its allocator grew the heap through `brk`, its
`getpid` came through the gate, and it exited through `exit_group`. That is the
step-5 gate proving itself against code nobody on this project wrote.

### The initial stack has to be Linux-shaped

A libc reads more than `argc`: `__init_libc` wants `argc, argv[], NULL, envp,
NULL, auxv`. Three auxv entries are load-bearing:

- `AT_PHDR`/`AT_PHENT`/`AT_PHNUM` - the static startup walks the program headers
  to find `PT_TLS` and set the thread pointer. Without them `%fs` is never set
  and the first thread-local access faults in a program that looked fine.
- `AT_PAGESZ` - becomes `libc.page_size`, which malloc and stdio divide by.
- `AT_RANDOM` - seeds the stack canary. Zeros would make the check pass
  trivially, so 16 bytes are derived from the tick counter instead. It is not
  real entropy and the code says so.

`AT_PHDR` is the mapped address of the header table, and it is computed through
the load bias: zero for `ET_EXEC`, and for an `ET_DYN` image the base the kernel
picked minus the image's lowest segment. The address is derived from the segment
that actually covers the table's file range rather than from `base + e_phoff`,
because a linker may emit a segment whose offset and vaddr do not agree - musl's
own binaries do exactly that in their last segment, so the naive form points a
few kilobytes into the wrong place.

`AT_BASE` is the interpreter's load bias and `AT_ENTRY` stays the program's own
entry: the task begins at the interpreter and `ld.so` jumps to `AT_ENTRY` when
it has finished. `AT_EXECFN` has to be a real string even for a program started
with no argv. musl sets a presence bit for it and then compares the pointer
without looking at it again, so a NULL there faults inside the loader before any
of the program runs - which is exactly what the first `PT_INTERP` boot did.

## Threads

A libc's second thread is where a single-threaded kernel starts lying, and three
lies had to go:

- **`clone(56)`.** Only the thread half: `CLONE_VM` or `-ENOSYS`, because a
  `fork()` answered with a shared address space would corrupt two processes at
  once, and copying a space is a separate job. The kernel is not given the
  function or its argument - the libc's own wrapper holds those - so what is
  copied is the *caller's saved register frame*, with `rax` zeroed and `rsp`
  replaced by the caller's stack. That is the Linux contract exactly: the child
  resumes at the instruction after the `syscall` and takes the child branch.
  The frame the syscall stub saves has nineteen qwords; `ctx_t` has twenty
  (the rbp slot the stub does not push), so the fields are copied by name. A
  `memcpy` between the two resumes a thread with every register shifted by one.
- **`%fs` is per thread.** The MSR is written by `arch_prctl(SET_FS)` *and*
  stored in the task; the scheduler reloads it on every switch. With only the
  MSR, the second thread's thread-locals land on the first thread's: `__thread`
  state looks shared, and a canary check or a `pthread_self()` reads someone
  else's.
- **`CLONE_CHILD_CLEARTID`.** `pthread_join` waits on a futex word that the
  dying thread zeroes and wakes. A kernel that never clears it makes join hang
  for ever, which looks like a scheduler bug and is not one.

`userland/muslthreads.c` is the test, and it is written so each failure is
visible: four threads increment a `__thread` counter and a mutex-guarded shared
one. Per-thread TLS is real only if every thread's `tls` reads 200 with the
value it started from at 0; the mutex is real only if `shared` comes out exactly
800; `join` is real only if all four return values arrive.

```
[threads] starting 4 threads, main tls=0
[thr] 200 iterations, tls=200 (borrowed 0)   x4
[threads] shared=800 joined=1600 main-tls=0 OK
```

### The stack that was not there

Neither musl program reached its own `main` at first, and the fault was a read
at `0x8010000000` - one past the top of the stack, in `__init_libc`, with
`rax` pointing at `__environ`. The cause was not in the libc path at all:
`ring3_spawn_elf` started tasks at `RING3_LIMIT - 16` and put *nothing* on the
stack. Programs of ours read argc and ignore the rest, so this had gone
unnoticed for the whole life of the kernel. A libc walks `envp` until it finds a
NULL: with no NULL to find, it reads off the top of the mapped stack and dies in
a place that looks nothing like the bug. Both spawn paths now build the same
Linux-shaped stack.

## stat, and the half of the address space nothing looked at

`stat` is three syscalls wearing one name: `stat` and `lstat` (4, 6), `fstat` (5)
and `newfstatat` (262), which is what a libc actually calls - musl answers
`stat()` with `newfstatat(AT_FDCWD, ...)` and, for a descriptor, with an empty
path, so the "ask about the thing behind the fd" form has to exist or `fstat`
never reaches the kernel. The struct is 144 bytes of x86-64 `struct stat`, layout
not ours to choose, including the padding after `st_gid` that puts `st_rdev` on an
8-byte boundary.

Everything it reports is true but minimal: one device, `st_blksize` 4096,
`st_blocks` at 512 a block, and timestamps from the boot tick counter. A console,
a pipe and a socket get their real types rather than "regular file" - a program
that stats stdout and is told it is a file will try to seek it. A relative path
with a directory descriptor is `-EOPNOTSUPP` instead of being quietly resolved
against the root, because a wrong answer here is worse than a refusal.

The first version of this failed with `-EFAULT` on a path string that was
obviously mapped, and the reason is the most Omi-shaped bug in the port:
`user_page_present` - the check every kernel-side read of user memory goes
through - looked only at PDPT slot 1, the high window. Every byte of a program
linked at 0x400000 read as unmapped. Nothing had noticed, because the only
low-half program that ever called a syscall taking a *path* was a libc, and the
only low-half programs before that took fds and buffers, which go through a
different check. The slot is now derived from the address being asked about.

```
[stat] ok   fstat on an open file ... [stat] ok   /etc/version is five bytes
[stat] ok   a missing file fails with ENOENT, a closed descriptor with EBADF
[stat] OK stat and fstat agree with the filesystem
```

## Signals: stored, remembered, never raised

`rt_sigaction` (13) and `rt_sigprocmask` (14) are implemented to the point a libc
needs at startup: handlers live in the address space (they are per process, like
`mm`), the blocked mask lives in the task (it is per thread, like `%fs`), both
can be set and read back, and neither is ever consulted. There is no signal
frame, no altstack, and nothing raises a signal - a fault kills the task instead
of calling into it. `pause` (34) returns `-EINTR` for the same reason: nothing
can arrive, so nothing can interrupt it. That is a documented gap, and the
minutes it saves are real: `pthread_create` calls into this path before it
creates anything.

`fcntl` (72) answers `F_GETFD`/`F_SETFD` and forwards `F_GETFL`/`F_SETFL` to the
open file description. `F_DUPFD` is `-EINVAL` - there is no `dup` yet, and a
made-up descriptor number would be worse than a refusal.

## fork: a copy, not a share

`fork` arrives two ways. musl issues syscall 57 directly, and a `clone` without
`CLONE_VM` means the same thing - both land in one function, because they are one
operation with two doors. The child resumes at the instruction after the syscall
with the same stack pointer, in a copied address space; its `rax` is 0 and its
parent's is the child's id.

The copy is `mm_clone`: walk both PML4 slots, and for every present page that is
user-accessible, copy the frame and map it in the child's space with the same
writable bit. Pages that are present but *not* user-accessible are the kernel's
own mappings inside the space - each space makes its own, so copying them would
duplicate the kernel's identity map once per process. The lazy regions travel
too, or a child faulting where its parent had already allocated would grow into
addresses the parent owns. Descriptors are shared, not copied: `fdtable_clone`
puts the same open files in the same slots with one more reference each, which is
why a child's write to a pipe reaches its parent's reader and a child's `close`
does not close the parent's.

The copy is deferred. `fork` maps the parent's writable pages **read-only in both
spaces** over the same frames and records how many spaces map each frame
(`kernel/cow.c`); the first store on either side takes a page fault, gets a private
frame, and the instruction retries. Pages that are already read-only - code,
rodata - are handed over as they stand, which is why forking a large program is
cheap without any further cleverness. The last space to drop a frame frees it;
`free_region` asks `cow_release` instead of freeing directly, so a frame outlives
the first parent that dies. Shared segments (`PTE_SHARED`, the GUI window) are
exempt in both directions: they are writable on purpose and copying one would
break two processes talking through it.

Measured on the fork test: 19 pages shared, 11 of them made read-only, **one real
copy per child** - the page it actually wrote. The addresses make it checkable
rather than asserted:

```
[fork] the inherited buffer is at 0x8000100000
[cow] pid=0x29 va=0x8000100000 old=0x156000 new=0x167000 refs=2->1
[cow] pid=0x29 faults=3 copies=1 steals=2 | bank shares=38 releases=42 live=0
```

That is the child faulting at the address the test printed, receiving a different
frame, and the parent keeping the original - the outcome test (the parent's copy
still says `parent`) is the second half of the proof, not the whole of it. A
"steal" is the same fault on the last holder: the write bit comes back with no
copy, which is what a fork-then-exec wants. `live=0` at exit is the leak check,
and `releases = shares + distinct frames` is the invariant behind it.

`mm_clone` likewise no longer copies; if the frame table is full it falls back to
copying eagerly, because a slower fork is a better failure than a wrong one.

**Refcounts are only as good as the lifetimes they survive.** One fork and one
write proves the copy path and nothing about the accounting, so `cowlives` runs
five lifetimes that break the easy case: a child two forks deep outliving its
parent, a child dying first, three generations where both ancestors go before the
youngest writes, four children writing the same inherited page, and a shared
segment that must *not* be copied. A sixth case is the one that found a bug:
`munmap` in the parent of a page a forked child still maps. `sys_vm_free` freed
the frame outright, so the child's next write went into whatever the allocator
handed that frame to next - invisible to the parent, and exactly the kind of
premature free a refcount is supposed to prevent. Every frame release now goes
through `cow_release` whatever path it takes (`free_region`, `munmap`, `mprotect`),
and teardown reports `kept=` for frames another space still maps.

The bank is audited at the teardown of any space that ever forked: every remaining
entry is checked against a bitmap of frames some live space actually maps. That
distinguishes a refcount that never dropped from a frame that is genuinely still
in use, which from the counter alone look identical:

```
[cow] pid=0x21 faults=0x1F copies=0x9 steals=0x16 | bank shares=0x13C releases=0x178 live=0x12 saturated=0
[mm] cow participant's space freed
[cow] audit live=0x0 entries=0x0 orphans=0x0 - every frame accounted for
```

`live=0x12` at the exit tally are the exiting process's own pages - frames its dead
children had released back to it - and they go when its space does. An `ORPHAN`
line means a frame is counted but mapped by nothing, which is a leak and fails the
run; the audit prints even when the bank is empty, because silence cannot be read
as a balance. Two things this cost: walking page tables once per entry stalled the
boot until it was one pass into a frame bitmap, and the repeat-fault detector had
to compare a fork generation - a page legitimately faults again after a *later*
fork re-protects it, so without that, correct behaviour shows up as `REPEAT-FAULT`.

`wait4` (61) is what makes the child collectable. There are no signals, so a
child's death cannot arrive as one: the parent blocks, and the kernel wakes it by
parent id when the child dies (`task_t.parent`, set at spawn). A pid matching no
child of this task is `-ECHILD` - the refusal a shell reads as "nothing left",
and the reason a mistaken wait cannot block forever.

The trap in all of this is the frame: it is laid out in `ctx_t` order, not in
interrupt push order, so rip is `frame[14]` and **rsp is `frame[17]`**. The first
version passed `frame[4]` - r11 - and the child faulted writing at 0x23e with a
stack pointer that was never a pointer.

```
[fork] ok   fork returns the child's pid to the parent
[fork] ok   the child's exit code came back as 7
[fork] ok   the child wrote through the pipe it inherited
[fork] ok   the child's write to a global stayed in the child
[fork] OK processes copy, descriptors share
```

## Acceptance

`userland/linuxtest.c`, 26 checks, all from ring 3 through the real instruction:
write returns a byte count; `getpid` agrees across both ABIs; `int $0x80` 34 is a
pipe while `syscall` 34 is `-ENOSYS`; pipe/write/read/close through Linux numbers;
`clock_gettime` monotonic and in range; `brk` grow plus writes through the whole
span; `mmap`/`munmap` writable end to end; `arch_prctl(SET_FS)` verified by
reading `%fs:0`; unknown number `-ENOSYS`.

Boot: `[linux3] 27/27 ok`, `[musl] OK`, `[threads] ... OK`, `[stat] OK` and `[fork] OK`, alongside `[pipe] 37/37`,
`[pipe3] 30/30`, `[rdy] 56/56`, `vfs 16/16`, `futex 3/3`, `[rfs] module files exposed=24 skipped=0`
with `[modtest] OK 15/15`, `[maptest] OK 24/24` (file-backed `mmap`, `MAP_FIXED` over a
reservation, a `PROT_NONE` page that kills the child that reads it, a store into a read-only page
that kills the child that writes it, and the three refusals), `[dyntest] OK 14/14` **twice** (a PIE run
through musl's `ld.so` with a library mapped out of the boot medium - once from the spawn list, once as
the target of an `execve`), `[exectest] OK 7/7` (a refused name leaves the caller running, a child
becomes another program with its argv and inherited descriptor intact, a PIE can be the target, and the
parent's own page survives its child's exec), `[epolltest] OK 24/24` (registration, `EEXIST` on a
second `ADD`, level triggering, the value handed back per member, `EPOLLHUP` from a closed peer
without asking for it, `EPOLLONESHOT` reporting once, a wait that really blocks until a child writes
200 ms later, a timeout that is a deadline, `EPOLLET` refused, a closed instance returning
`EBADF`), no panic, no "killed user task" beyond the isolation probe that asks for it.

`epoll` is poll with the set kept. There is no event queue and no tree: a wait asks every member
through the same `fd_poll()` that `poll()` uses, so the cost is the size of the set, not the number
ready, and that is what the code does rather than what the name promises. What registration buys is
what an event loop needs - build the set once instead of rebuilding it every iteration. Edge
triggering is refused (`-EINVAL`) rather than served as level, because a program that asked for edges
and got levels re-arms nothing and spins, and the resulting stall would look like a kernel fault.

`nanosleep` (35) and `clock_nanosleep` (230) arrived because a test failed: `[epolltest]` gave a child
200 ms to write and the child wrote at once, because `nanosleep` was `-ENOSYS` and every `sleep()` in
this system had been returning immediately. The check now asserts the sleep itself (measured >= 180 ms
for a 200 ms request), so a stub cannot come back quietly.

`execve` (59) replaces the process: image, stack, areas and the page-table root, while the descriptor
table and the pid stay. The old space is not freed in place - the call is running on its kernel stack -
so it goes on the zombie list and the scheduler reaps it once CR3 has moved. Two limits: `envp` is
accepted and ignored, because nothing here has an environment to hand over, and a call from a space
shared with a sibling thread is refused with `-EINVAL` rather than ending that thread.

Protection is checked before a page is filled: a fault the range does not allow is a violation, not a
demand to page in. On x86 there is no write-only page, so `PROT_WRITE` implies read, as it does under
Linux. One difference worth knowing: a range painted `PROT_NONE` drops its bytes, so making it
accessible again gives zeroes. Linux preserves them. Nothing here records a frame after its PTE is
cleared, so the alternative was leaking every page behind a guard nobody lifts.