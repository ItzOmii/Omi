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

PROGS := cat.elf httpget.elf polltest.elf

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
