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

## 6b - file-backed `MAP_PRIVATE` (next)

`linux_mmap` currently refuses anything without `MAP_ANONYMOUS`. What `ld.so`
needs is a mapping whose pages come from the file's bytes and become private on
first write - which is the COW machinery already in `kernel/cow.c`, with the
module blob as the fill source instead of the parent's frame.

Design constraints, so the mapping cannot become a second memory subsystem:

- read-only PTE from the start; a write fault allocates a frame, copies one page
  from the blob, and clears the write-protect - the same path a `fork` COW fault
  takes;
- the area records its file and offset, so `munmap` releases everything through
  `cow_release` like any other area (the `sys_vm_free` bug is the precedent: a
  frame released behind the pool's back is a leak the audit would blame on
  someone else);
- offset and length must be page-aligned down / rounded up, and a map past EOF
  is `-EINVAL`, not a zero-filled tail - silently inventing bytes is how a
  corrupt library gets loaded without a diagnostic.

## 6c - `PT_INTERP` and `AT_BASE` (after that)

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