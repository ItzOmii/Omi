# Omi

Omi is a fork of [vib-OS](https://github.com/viralcode/vib-OS) — same custom 64-bit kernel,
same from-scratch userspace — with a different face and a few things the base tree does not
have: networking, sound, a browser, a ring-3 process model with copy-on-write `fork`, and a flat,
paper-coloured shell.

Everything here is x86_64 and lives in [`vib-os-x86_64/`](vib-os-x86_64/). The ARM/RISC-V trees
are untouched upstream code.

![Omi desktop](vib-os-x86_64/screenshots/omi-desktop.png)

**Landing page:** [`docs/index.html`](docs/index.html) — the pitch, the stack, and eight unretouched
1280×800 captures taken in QEMU. It is static HTML with no javascript; open the file directly or
serve the folder.

## What Omi adds

**Shell.** Flat, shadowless window chrome — cream paper, a `#FFC23D` accent,
hairline borders, Bricolage Grotesque + JetBrains Mono. Midnight is the boot appearance. A
Windows-style taskbar sits at the bottom with a start menu; the Omi mark is the start button.
Iconography is [Remix Icon](https://icon-sets.iconify.design/ri/), baked to bitmaps by
`vib-os-x86_64/tools/gen_ri_icons.py` so the kernel needs no font renderer for them.
Limine boots straight through — `timeout: 0`, no menu, no countdown.

**Networking.** A real stack, no shims: e1000 driver, ARP, IPv4, ICMP, UDP, DHCP, DNS and TCP,
with an HTTP/1.1 client on top (`kernel/net/`). The network widget reports link state and whether
`tcp + http` actually answer; Omi Net uses the same stack for pages.

**Omi Net.** A browser window with a typed address bar, tabs, scrolling and a status strip
(`http 200 · lines · scroll`). Pages are fetched over TCP and rendered by Gumbo plus a CSS box model
of our own (`kernel/gui/omiweb.c`), with stb_truetype text, decoded images, clickable links and
tables. HTTPS works: TLS 1.2 through BearSSL with a pinned root bundle (`kernel/net/roots.c`).

**Processes.** GDT + TSS, a 100 Hz PIT, and a round-robin scheduler over 96 task slots with
16 KB kernel stacks (`kernel/kernel_task.c`). The task manager window lists every task with state,
ticks, switches and a CPU-share bar, and is reachable with Ctrl+Alt+Del or from the taskbar's
right-click menu.

**User mode.** Each program gets its own PML4 — the kernel's tables copied in supervisor-only, one
slot rebuilt for the program — entered by an `iretq` into CPL 3 and served through an `int 0x80`
gate at DPL 3. Programs are ELF files on the boot medium, loaded by `kernel/elf.c`. Scheduling is
preemptive for ring 3: every suspended task is an interrupt frame, and IRQ0 switches it away a
hundred times a second. The compositor acts as a server — a process sends draw commands and receives
mouse and key events — and `SYS_SHM_GET`/`SYS_SHM_ATTACH` map the same frames into two address
spaces.

**Processes in ring 3.** A program is a real musl binary: libc sets itself up from the auxv, threads
run on `clone` with futexes, `fork` copies the address space copy-on-write behind a refcounted page
bank, and `wait4` reaps. A process opens files by path — every boot module is registered read-only
under its boot name, so a library can be read from inside the system that loaded it — and maps one
into its own space with a file-backed `MAP_PRIVATE` mapping.

**Dynamic linking.** A position-independent program that names an interpreter in `PT_INTERP` runs.
The kernel picks the addresses: the program at the base of the user window, musl's `ld.so` at +1 MB
in the same space, both registered so `mmap` will not hand a library those pages. The task starts at
the interpreter's entry with `AT_ENTRY` naming the program and `AT_BASE` naming the interpreter, and
`ld.so` does everything after that through the same `open`/`mmap` a program would use: `[dyntest] OK
14/14` on a real boot, including a `printf` from libc, a `malloc` that grows, and a call into another
shared object at an address `ld.so` chose. The mechanics, and the three kernel bugs it exposed, are
in `docs/dynamic-linking.md`.

**Sound.** Intel HDA output with a streaming API (`hda_stream_start/write/free/stop`) and an MP3
decoder (minimp3) in `kernel/media/`. The media player streams the track from disk while it plays,
shows elapsed/total time and a progress bar, and does not stall the compositor while decoding.

**Apps.** Calculator, notepad, media player, Omi Net, Files and the task manager, plus toggleable
clock / weather / network widgets.

## Building

```sh
sudo apt install build-essential xorriso mtools gcc nasm
cd vib-os-x86_64
make            # -> omi.iso
```

Run it:

```sh
qemu-system-x86_64 -m 512 -cdrom omi.iso -boot d -vga std \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev sdl,id=snd -device intel-hda -device hda-output,audiodev=snd
```

`-device e1000` is what gives Omi a network (QEMU user-mode networking hands out `10.0.2.15`).
`hda-output` is correct for most real machines; `hda-duplex` only if you have a capture endpoint
(most desktops do not, and duplex then fails at init).

## What does not work yet

- **Nothing can ask for a program to be run.** `execve` works - a program replaces its own image,
  with argv arriving in the new space and its descriptors carried across, and a PIE can be the target
  so the path runs through `ld.so` as well (`[exectest] OK 7/7`). What is missing is the caller: there
  is no shell and no `PATH`, so the kernel's spawn list is still how a session begins. Two limits on
  the call itself: `envp` is ignored because nothing in this system has an environment, and exec from
  a space shared with another thread is refused rather than ending that thread.
- **Shared file mappings are missing.** `MAP_SHARED` over a file is `-ENOSYS`: there is no write-back
  path, so a shared mapping would be a lie about durability. `PROT_NONE` is implemented - a read of a
  guard faults, and so does a store into a read-only page (`[maptest] OK 24/24`), with one difference
  from Linux: the bytes under a no-access span are dropped rather than preserved.
- **No microphone capture** on real hardware — no capture endpoint is exposed by most onboard HDA.
- **The shell's apps are still kernel-resident windows**, as in upstream vib-OS. Programs run outside
  the kernel, but Calculator, Notepad, the browser and the file manager are structs and draw functions
  compiled in.
- **No `exec` from a path.** A program is spawned from a hardcoded list matched by module name, so a
  loaded module is not a runnable file.
- **No TCP teardown, retransmission, server sockets or UDP.** One FIN ends a connection; `bind`/
  `listen`/`accept` do not exist.
- **Slow HTTP origins look slow.** Connect timeouts are 1.5 s → 3.5 s → 5 s across three attempts
  (fresh source port each time), so a site that takes 11 s to answer will fail rather than hang.

## Sample media

`vib-os-x86_64/assets/sample.{mp3,mp4}` are generated placeholders (ffmpeg sine tones and a test
pattern), committed so the media player has something to open. Swap in your own files and rebuild.

## Upstream

Forked from [viralcode/vib-OS](https://github.com/viralcode/vib-OS) at `73b6429`. Changes are
confined to `vib-os-x86_64/` and sit as 26 commits on top of upstream — see `git log --stat` or the
compare view against `viralcode/vib-OS:main`.