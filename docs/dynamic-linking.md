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
given for it (`kernel/ring3.c`, `[rfs] module files exposed=18`). The entry
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
- **`PROT_NONE` is not implemented** - reservations come back readable. `ld.so`
  only uses them as placeholders it maps over, so nothing depends on it yet.
  Recorded as a gap, not as a design.

The test compares the same page fetched two ways, `read()` and the mapping, and
requires the page to be non-zero, so a zero fill of the right length fails.

## 6c - `PT_INTERP` and `AT_BASE` (next)

`kernel/elf.c` rejects everything that is not `ET_EXEC` today (`not a static
executable`). It needs: `ET_DYN` loaded at a chosen base, `PT_INTERP` read from
the file, the interpreter loaded as an image, and the auxv extended with
`AT_BASE` (interpreter) while `AT_ENTRY` stays the program's own entry. The
program's `AT_PHDR` must point at the program's phdrs, not the interpreter's -
`ld.so` uses it to find the dynamic section it is about to process.

The honest failure to get right: a `NEEDED` library that is not on the boot
medium must produce `ld.so`'s own "Error loading shared library" and a non-zero
exit, not a kernel panic and not a silently loaded program that crashes three
instructions later.