# Standalone userspace programs for Omi.
#
# Deliberately independent of the kernel build: its own flags, its own linker
# script, its own artifacts, and no include path into the kernel. The only
# contract with the kernel is the syscall ABI, which is the point - a shared
# header would let a wrong interface compile happily in both places.

# `:=`, not `?=`: make defines CC=cc itself, so ?= would never fire.
# A command-line `make CC=...` still overrides these.
CC      := clang
LD      := ld.lld

CFLAGS  := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector \
           -fno-stack-check -fno-lto -fno-PIC -fno-builtin -m64 -march=x86-64 \
           -mno-red-zone -mcmodel=large -O2 -Wall -Wextra
LDFLAGS := -nostdlib -static -z noexecstack --no-dynamic-linker

PROGS := cat.elf httpget.elf polltest.elf spin.elf pipetest.elf linuxtest.elf
# muslhello is the only program here not built by our own freestanding toolchain -
# it is the proof that a real libc works. Optional: skip it (with a note) when
# musl-gcc is not installed rather than failing the whole build.
ifneq ($(shell command -v musl-gcc 2>/dev/null),)
PROGS += muslhello.elf
PROGS += muslthreads.elf
PROGS += muslstat.elf
PROGS += muslfork.elf
PROGS += cowlives.elf
PROGS += libprobe.elf
PROGS += libmap.elf
PROGS += dyntest.elf
PROGS += exectest.elf
PROGS += epolltest.elf
PROGS += memfdtest.elf
PROGS += socktest.elf
PROGS += exectarget.elf
# Not a program: a shared object, built with musl-gcc so its relocations and its
# NEEDED entry are real. Copied to the ISO's /lib and loaded as a boot module.
PROGS += libgreet.so
endif

all: $(PROGS)

cat.elf: cat.o start.o cat.ld
	$(LD) $(LDFLAGS) -T cat.ld -o $@ cat.o start.o
	@echo "built $@"

httpget.elf: httpget.o start.o cat.ld
	$(LD) $(LDFLAGS) -T cat.ld -o $@ httpget.o start.o
	@echo "built $@"

polltest.elf: polltest.o start.o cat.ld
	$(LD) $(LDFLAGS) -T cat.ld -o $@ polltest.o start.o
	@echo "built $@"

spin.elf: spin.o start.o cat.ld
	$(LD) $(LDFLAGS) -T cat.ld -o $@ spin.o start.o
	@echo "built $@"

cowlives.elf: cowlives.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping cowlives"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

libprobe.elf: libprobe.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping libprobe"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

libmap.elf: libmap.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping libmap"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

socktest.elf: socktest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping socktest"; exit 0; }
	@musl-gcc -O2 -static -o $@ $< && echo "built $@" || echo "socktest build failed"

memfdtest.elf: memfdtest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping memfdtest"; exit 0; }
	@musl-gcc -O2 -static -o $@ $< && echo "built $@" || echo "memfdtest build failed"

epolltest.elf: epolltest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping epolltest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

exectest.elf: exectest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping exectest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

exectarget.elf: exectarget.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping exectarget"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

# A PIE with PT_INTERP: this one does not run until the kernel has mapped the
# interpreter and ld.so has relocated it. Linked against libgreet.so so the
# loader has a second file to find, map and resolve.
dyntest.elf: dyntest.c libgreet.so
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping dyntest"; exit 0; }
	musl-gcc -pie -O2 -o $@ $< -L. -lgreet

# -fPIC and a real dynamic section: the point of this file in the ISO is that the
# loader has to cope with relocations, not that the code does anything clever.
libgreet.so: dl/libgreet.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping libgreet"; exit 0; }
	musl-gcc -shared -fPIC -O2 -o $@ $<
	@echo "built $@ (shared object, $(shell stat -c%s $@ 2>/dev/null || echo 0) bytes)"

muslfork.elf: muslfork.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping muslfork"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

muslstat.elf: muslstat.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping muslstat"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<

muslthreads.elf: muslthreads.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping muslthreads"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@ (musl static, pthreads)"

muslhello.elf: muslhello.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping muslhello"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@ (musl static)"

linuxtest.elf: linuxtest.o start.o cat.ld
	$(LD) $(LDFLAGS) -T cat.ld -o $@ linuxtest.o start.o
	@echo "built $@"

linuxtest.o: linuxtest.c
	$(CC) $(CFLAGS) -c -o $@ $<

pipetest.elf: pipetest.o start.o cat.ld
	$(LD) $(LDFLAGS) -T cat.ld -o $@ pipetest.o start.o
	@echo "built $@"

pipetest.o: pipetest.c
	$(CC) $(CFLAGS) -c -o $@ $<

spin.o: spin.c
	$(CC) $(CFLAGS) -c -o $@ $<

polltest.o: polltest.c
	$(CC) $(CFLAGS) -c -o $@ $<

httpget.o: httpget.c
	$(CC) $(CFLAGS) -c -o $@ $<

cat.o: cat.c
	$(CC) $(CFLAGS) -c -o $@ $<

start.o: start.S
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(PROGS)

.PHONY: all clean
