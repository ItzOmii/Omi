# Dynamic linking

Step 6 of the Chrome roadmap. The goal is to run a dynamically linked musl
program: a PIE main executable with `PT_INTERP`, a shared object it did not know
about at link time, and a libc that arrives as a mapping rather than as part of
the program.

## Division of labour

The kernel does **no relocations**. `ld.so` is a user-space program and it is
perfectly capable of walking a dynamic section, so the kernel's whole job is the
three things user space cannot do for itself:

1. **Name a file.** `ld.so` opens libraries by path. Until step 6a nothing on
   this system could open a file that was not already an argv path.
2. **Map a file.** `ld.so` maps each library's `PT_LOAD` segments from its own
   descriptor. Anonymous `mmap` is not enough.
3. **Hand over to the interpreter.** `PT_INTERP` means the program's entry point
   is *not* where execution starts: the kernel loads the interpreter, puts the
   program's entry in `AT_ENTRY`, the interpreter's base in `AT_BASE`, and jumps
   to the interpreter. Everything after that is musl's code.

That split is why this is three steps and not thirty. Relocation types, symbol
resolution, lazy binding, `DT_NEEDED` search order, `%fs` TLS setup for a shared
object - none of it is kernel work, and implementing any of it in the kernel
would be a second, worse libc.

## What a musl dynamic binary actually asks for

Measured on the build host, not remembered:

```
$ readelf -l dynmain | grep interp
      [Requesting program interpreter: /lib/ld-musl-x86_64.so.1]
$ readelf -d dynmain
 0x0000000000000001 (NEEDED)  Shared library: [libgreet.so]
 0x0000000000000001 (NEEDED)  Shared library: [libc.so]
```

So: the main image is `ET_DYN` (PIE), it needs an interpreter by absolute path,
and the interpreter itself satisfies `libc.so` (musl's `ld.so` *is* libc). One
shared object of our own and no other dependencies - a deliberately small
target, because every dependency would be another failure mode mixed into the
same test.

## 6a - boot modules are files (done)

Every Limine module is registered in the RAM file table under the path Limine was
given for it (`kernel/ring3.c`, `[rfs] module files exposed=21 skipped=0`). The entry
points straight at the module blob:

- **no copy** - `rfile_t.data` is the module's address, so a 16 KB library costs
  no heap;
- **`cap == size`** - the old 4 KB heap cap does not apply, so `read()` runs to
  the module's real end;
- **read-only** - `open(O_WRONLY)` and `write()` both return `-EROFS`. These
  bytes are the bootloader's image; truncating one would corrupt a file other
  processes are running from.

Verified by `userland/libprobe.c`, 15 checks, driven from ring 3 through real
syscalls:

```
[modtest] OK 15/15 (module 16048 bytes, 31 chunks of 512)
```

The size in that line is the cross-check: 16,048 is `userland/libgreet.so` on
the host, byte for byte. The test also requires the file's own symbol string
(`greet`) to appear in the bytes it read, so a plausible-looking length with
wrong contents still fails, and it opens `/hello.txt` afterwards to prove the
seeded RAM files were not shadowed.

## 6b - file-backed `MAP_PRIVATE` (done)

`linux_mmap` now takes the full Linux argument list and chooses between two
kinds of region:

```
[maptest] OK 17/17 (file-backed MAP_PRIVATE, MAP_FIXED over a reservation, refusals)
```

- **Pages are copied from the blob, not aliased to it.** The blob is bootloader
  memory owned by nobody: an aliased page could never be released through the
  refcount bank, so the audit would have needed an exception for frames it never
  owned. Copied frames are ordinary frames and leave through `cow_release`.
- **The area records the source, the fill happens in `vm_fault_in`.** No new
  fault path - the anonymous one grew a branch. That also keeps `fork` correct
  for free, since the child inherits the area and copies frames like any other.
- **`MAP_FIXED` releases what is in the way**, then drops any area overlapping
  the target range whole; its remainder becomes an unmapped fault, which is what
  the `PROT_NONE` reservation was standing in for. A hint allocation steps over
  live regions rather than colliding with a fixed mapping.
- **Offsets are page-aligned and must start before EOF**; a length whose last
  page runs past EOF is legal and reads zero there. The earlier plan said "map
  past EOF is EINVAL" - that was wrong, and reading musl's `ldso` showed why:
  it maps `(filesz + off + 4095) & -4096`, so every segment that is not a page
  multiple maps a page past EOF. Refusing that would refuse every library.
- **`MAP_SHARED` over a file is `-ENOSYS`** - nothing can write a frame back into
  a boot module, and a shared mapping that is silently private is worse.
- **`PROT_NONE` was not implemented** at the time this step was written:
  reservations came back readable. It is implemented now - the area carries the
  Linux protection bits, a fault that the range does not allow is a violation
  rather than a fill, and `[maptest]` checks both directions from a child
  process, because the assertion is that the child dies.

The test compares the same page fetched two ways, `read()` and the mapping, and
requires the page to be non-zero, so a zero fill of the right length fails.

## 6c - `PT_INTERP` and `AT_BASE` (done)

Measured on a boot of this tree: `[dyntest] OK 14/14`, exit code 0. The program is
a PIE linked against `libgreet.so` with three real dependencies on the loader -
`printf` from libc, `malloc` that grows, and a call into a second file - and it
reports where each image actually is:

```
[dyntest] program text 0x8000001110, libgreet text 0x80014020f0, data 0x8000004008
[dyntest] OK 14/14
```

`kernel/elf.c` now accepts `ET_EXEC` and `ET_DYN`. An `ET_DYN` image is loaded at
a base the kernel chooses (`RING3_USER_BASE` for the program), and the load bias
is `base - lowest_segment_vaddr`, which is what makes a PIE's `p_vaddr` values
absolute. `PT_INTERP` is read from the file, the interpreter is loaded as a
second image at `RING3_USER_BASE + 1 MB`, and both ranges are registered as
areas so the `mmap` hint walk will not hand a library the loader's pages. The
task starts at the interpreter's entry; `AT_ENTRY` stays the program's, `AT_BASE`
names the interpreter, and `AT_PHDR` is the program's header table as mapped.

`AT_PHDR` is computed from the segment covering the table's file range, not from
`base + e_phoff`. The two agree only while every segment has `vaddr == offset`,
and musl's binaries break that in their last segment.

### The three bugs this exposed

Each one was a real defect in earlier work, invisible until a libc ran:

1. **`RFILE_MAX` was 24, and the medium now carries 24 modules plus three seeded
   RAM files.** The three that did not fit were the last ones listed, so
   `libgreet.so` was silently unopenable: `open` returned `-ENOSPC` and the
   failure surfaced far away. Raised to 48 - the cap is slots, not memory.
2. **`AT_EXECFN` was set to NULL for programs spawned with no argv.** musl tests
   the presence bit and then `strncmp`s the pointer against `"/proc/"` without
   checking it, so `ld.so` faulted at address 0 in `strncmp` before anything ran.
   A name is now always written to the stack for that entry.
3. **`MAP_FIXED` dropped whole overlapping areas instead of unmapping the range.**
   mallocng reserves the heap with `brk` and then `MAP_FIXED`s a one-page
   `PROT_NONE` guard over the first page of it; dropping the whole area destroyed
   the heap, and the allocator's next write landed in a hole. Now the range is
   unmapped exactly: areas are trimmed or split in two, file windows and all.

### What is still not there

- A range painted `PROT_NONE` loses its bytes when it is made accessible again.
  Linux preserves them; nothing here records a frame once its PTE is cleared, so
  keeping the pages would leak every one of them. A guard is never taken back,
  which is why this has not mattered yet - and it is a difference, not a plan.
- The interpreter's base is a kernel constant; `dlopen` cannot choose it.
- `execve` now exists, and it runs through this loader: a static or position-independent program can
  be its target, and `[exectest]` execs both kinds on every boot. What it does not carry is an
  environment (`envp` is accepted and ignored), and it refuses rather than ends a sibling thread when
  the space is shared.
- A library missing from the boot medium is the loader's error to report -
  `ld.so` prints "Error loading shared library" and exits 127 - not the kernel's.
  The loader does refuse cleanly, before starting anything, when the *interpreter*
  named by `PT_INTERP` is not on the medium.