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
PROGS += tlsget.elf
PROGS += surftest.elf
PROGS += vmtest.elf
PROGS += shmtest.elf
PROGS += surfbig.elf
PROGS += unixpair.elf
PROGS += scmtest.elf
PROGS += unixname.elf
PROGS += credtest.elf
PROGS += formtest.elf
PROGS += imgtest.elf
PROGS += memtest.elf
PROGS += omirender.elf
PROGS += surfexit.elf
# The browser's ring-3 half: TLS, Gumbo, layout, paint.
PROGS += ominet.elf
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

# ---- TLS, in a program ---------------------------------------------------
# BearSSL is the kernel's TLS library; here it is compiled against musl instead,
# so the same code runs in ring 3 with no kernel help beyond the syscalls. Built
# as one archive because 277 files recompiled per program would dominate the
# build, and cached because none of them change when tlsget.c does.
BEARSSL     := ../kernel/lib/bearssl
BEARSSL_SRC := $(wildcard $(BEARSSL)/src/*/*.c)
BEARSSL_INC := -I$(BEARSSL)/inc -I$(BEARSSL)/src
BEARSSL_LIB := build/bearssl_user/libbearssl.a

$(BEARSSL_LIB): $(BEARSSL_SRC)
	@mkdir -p build/bearssl_user/obj
	@echo "compiling BearSSL for musl ($$(echo $(BEARSSL_SRC) | wc -w) files)"
	@echo $(BEARSSL_SRC) | tr ' ' '\n' | xargs -P4 -I{} sh -c \
	  'o=$(CURDIR)/build/bearssl_user/obj/$$(echo {} | sed "s|$(BEARSSL)/src/||; s|/|_|g; s|\\.c$$|.o|"); \
	   musl-gcc -O2 $(BEARSSL_INC) -c {} -o $$o || exit 255' || { echo "BearSSL build failed"; rm -f $@; exit 1; }
	@ar rcs $@ build/bearssl_user/obj/*.o
	@echo "built $@ ($$(stat -c%s $@) bytes)"

# The root store, compiled as the data file it is: kernel/net/roots.c has no
# kernel dependency, and shim/types.h keeps its one include honest.
roots_user.o: ../kernel/net/roots.c shim/types.h
	@musl-gcc -O2 -Ishim -I../kernel/include -c -o $@ $<

# Gumbo, compiled for musl. This is the same parser the kernel links, from the
# same sources: moving the renderer out of the kernel is a move, not a second
# HTML implementation that can disagree with the first.
GUMBO_USR  := ../kernel/lib/gumbo
GUMBO_SRCS := $(wildcard $(GUMBO_USR)/src/*.c)
GUMBO_LIB  := build/gumbo_user/libgumbo.a

$(GUMBO_LIB): $(GUMBO_SRCS)
	@mkdir -p build/gumbo_user/obj
	@echo "compiling Gumbo for musl ($$(echo $(GUMBO_SRCS) | wc -w) files)"
	@echo $(GUMBO_SRCS) | tr ' ' '\n' | xargs -P4 -I{} sh -c \
	  'o=$(CURDIR)/build/gumbo_user/obj/$$(basename {} .c).o; \
	   musl-gcc -O2 -I$(CURDIR)/$(GUMBO_USR)/src -c {} -o $$o || exit 255' || { echo "Gumbo build failed"; rm -f $@; exit 1; }
	@ar rcs $@ build/gumbo_user/obj/*.o

# omilayout.c is the layout engine that used to run in the kernel, moved here
# rather than rewritten: one copy in the tree, and its CSS subset comes with it.
ominet.elf: ominet.c omifont.c omifont.h imgdec.c imgdec.h stbimg.c ../kernel/include/buildstamp.h fetch.c fetch.h omitext.h glyphs.h omilayout.c omilayout.h roots_user.o $(BEARSSL_LIB) $(GUMBO_LIB)
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping ominet"; exit 0; }
	@musl-gcc -O2 -static -no-pie $(BEARSSL_INC) -Ishim -I$(GUMBO_USR)/src -I../kernel/include/stb -o $@ \
	  ominet.c omifont.c imgdec.c stbimg.c fetch.c omilayout.c roots_user.o $(BEARSSL_LIB) $(GUMBO_LIB) \
	  && echo "built $@ ($$(stat -c%s $@) bytes)"

tlsget.elf: tlsget.c roots_user.o $(BEARSSL_LIB)
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping tlsget"; exit 0; }
	@musl-gcc -O2 -static -no-pie $(BEARSSL_INC) -Ishim -o $@ tlsget.c roots_user.o $(BEARSSL_LIB) \
	  && echo "built $@ ($$(stat -c%s $@) bytes)"

# The address window itself: reserve more than the old 256 MB and prove both
# ends of it are real, because every engine that reserves a gigabyte depends on
# this and a wrong constant shows up as a fault deep inside the engine.
# Surfaces bigger than a demo window: the client window size a browser needs.
memtest.elf: memtest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping memtest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

imgtest.elf: imgtest.c imgdec.c imgdec.h
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping imgtest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ imgtest.c imgdec.c
	@echo "built $@"

formtest.elf: formtest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping formtest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $< omilayout.c $(wildcard $(CURDIR)/../kernel/lib/gumbo/src/*.c) -I../kernel/lib/gumbo/src
	@echo "built $@"

credtest.elf: credtest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping credtest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

unixname.elf: unixname.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping unixname"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

scmtest.elf: scmtest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping scmtest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

unixpair.elf: unixpair.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping unixpair"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

surfbig.elf: surfbig.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping surfbig"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

# Shared regions: the hand-off primitive between a browser and its renderers.
shmtest.elf: shmtest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping shmtest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

vmtest.elf: vmtest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping vmtest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

# The first program that paints its own pixels: a surface, presented whole and
# then by damage rectangle. Plain musl, no BearSSL, so it builds in one step.
surftest.elf: surftest.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping surftest"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

# The rasteriser, in ring 3: its own glyphs (generated from the kernel's table),
# its own clip, its own word wrap. Depends on the generated header, so editing a
# glyph in font.c rebuilds it.
omirender.elf: omirender.c glyphs.h
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping omirender"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ omirender.c
	@echo "built $@"

glyphs.h: ../tools/gen_glyphs.py ../kernel/gui/font.c
	python3 ../tools/gen_glyphs.py

# A window whose process exits: proves the kernel reaps what a dead task owned.
surfexit.elf: surfexit.c
	@command -v musl-gcc >/dev/null || { echo "musl-gcc missing: skipping surfexit"; exit 0; }
	musl-gcc -static -O2 -no-pie -o $@ $<
	@echo "built $@"

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
	rm -rf build

.PHONY: all clean
