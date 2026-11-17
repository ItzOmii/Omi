# UEFI Demo OS Makefile
# Builds a bootable x86_64 kernel using Limine bootloader

# Toolchain - use cross compiler if available, otherwise try clang
CC := clang
LD := ld.lld

# Check for x86_64-elf-gcc
CROSS_CC := $(shell which x86_64-elf-gcc 2>/dev/null)
CROSS_LD := $(shell which x86_64-elf-ld 2>/dev/null)
ifneq ($(CROSS_CC),)
    CC := x86_64-elf-gcc
    LD := x86_64-elf-ld
endif

# Directories
KERNEL_DIR := kernel
BUILD_DIR := build
ISO_ROOT := iso_root

# Output files
KERNEL := $(BUILD_DIR)/kernel.elf
ISO := omi.iso

# Compiler flags
CFLAGS := -target x86_64-unknown-none-elf \
          -ffreestanding \
          -fno-stack-protector \
          -fno-stack-check \
          -fno-lto \
          -fno-PIC \
          -m64 \
          -march=x86-64 \
          -mno-red-zone \
          -mcmodel=kernel \
          -I$(KERNEL_DIR)/include \
          -I$(KERNEL_DIR)/lib/bearssl/port \
          -I$(KERNEL_DIR)/lib/bearssl/inc \
          -Wall -Wextra \
          -O2 \
          -g

# Use GCC-compatible flags if using cross compiler
ifneq ($(CROSS_CC),)
CFLAGS := -ffreestanding \
          -fno-stack-protector \
          -fno-stack-check \
          -fno-lto \
          -fno-PIC \
          -m64 \
          -march=x86-64 \
          -mno-red-zone \
          -mcmodel=kernel \
          -I$(KERNEL_DIR)/include \
          -I$(KERNEL_DIR)/lib/bearssl/port \
          -I$(KERNEL_DIR)/lib/bearssl/inc \
          -Wall -Wextra \
          -O2 \
          -g
endif

# Linker flags
LDFLAGS := -nostdlib \
           -static \
           -z max-page-size=0x1000 \
           -T $(KERNEL_DIR)/linker.ld

# compiler-rt soft-float helpers (minimp3 uses float maths, kernel is -mno-sse)
BUILTINS := $(shell ls /usr/lib/llvm-*/lib/clang/*/lib/linux/libclang_rt.builtins-x86_64.a 2>/dev/null | head -1)

# Source files
SOURCES := $(KERNEL_DIR)/boot/limine_boot.c \
           $(KERNEL_DIR)/lib/string.c \
           $(KERNEL_DIR)/lib/builtins_shim.c \
           $(KERNEL_DIR)/mm/kmalloc.c \
           $(KERNEL_DIR)/mm/mmio.c \
           $(KERNEL_DIR)/mm/pmm.c \
           $(KERNEL_DIR)/cow.c \
           $(KERNEL_DIR)/walltime.c \
           $(KERNEL_DIR)/fs/vfs.c \
           $(KERNEL_DIR)/drivers/framebuffer.c \
           $(KERNEL_DIR)/drivers/idt.c \
           $(KERNEL_DIR)/drivers/wc.c \
           $(KERNEL_DIR)/drivers/ps2.c \
           $(KERNEL_DIR)/drivers/pci.c \
           $(KERNEL_DIR)/drivers/e1000.c \
           $(KERNEL_DIR)/net/net.c \
           $(KERNEL_DIR)/drivers/hda.c \
           $(KERNEL_DIR)/drivers/acpi.c \
           $(KERNEL_DIR)/drivers/usb.c \
           $(KERNEL_DIR)/drivers/usb_hid.c \
           $(KERNEL_DIR)/drivers/usb_xhci.c \
           $(KERNEL_DIR)/drivers/usb_ehci.c \
           $(KERNEL_DIR)/gui/font.c \
           $(KERNEL_DIR)/gui/terminal.c \
           $(KERNEL_DIR)/gui/desktop_mgr.c \
           $(KERNEL_DIR)/gui/window.c \
           $(KERNEL_DIR)/gui/compositor.c \
           $(KERNEL_DIR)/gui/omiweb.c \
           $(KERNEL_DIR)/gui/ttf.c \
           $(KERNEL_DIR)/kernel_task.c \
           $(KERNEL_DIR)/gdt.c \
           $(KERNEL_DIR)/ring3.c \
           $(KERNEL_DIR)/elf.c \
           $(KERNEL_DIR)/fs/fd.c \
           $(KERNEL_DIR)/fs/socket.c \
           $(KERNEL_DIR)/fs/pipe.c \
           $(KERNEL_DIR)/fs/epoll.c \
           $(KERNEL_DIR)/fs/shfile.c \
           $(KERNEL_DIR)/fs/fd_opaque_check.c \
           $(KERNEL_DIR)/fs/socket_selftest.c \
           $(KERNEL_DIR)/fs/readiness_selftest.c \
         $(KERNEL_DIR)/fs/preempt_selftest.c \
           $(KERNEL_DIR)/gui/webimg.c \
           $(KERNEL_DIR)/gui/icons_ri.c \
           $(KERNEL_DIR)/net/tcp.c \
           $(KERNEL_DIR)/net/tls.c \
           $(KERNEL_DIR)/net/roots.c \
           $(KERNEL_DIR)/net/http.c \
           $(KERNEL_DIR)/media/media.c \
           $(KERNEL_DIR)/media/image.c \
           $(KERNEL_DIR)/media/modules.c \
           $(KERNEL_DIR)/media/mp3.c \
           $(KERNEL_DIR)/media/picojpeg.c \
           $(KERNEL_DIR)/media/hd_wallpaper_landscape.c \
           $(KERNEL_DIR)/media/hd_wallpaper_nature.c \
           $(KERNEL_DIR)/media/hd_wallpaper_city.c

# Object files
BEARSSL := $(KERNEL_DIR)/lib/bearssl
BEARSSL_ALL := $(wildcard $(BEARSSL)/src/*/*.c)
# Everything that only exists for big-endian PowerPC/x86-64 extras, plus server
# and client-certificate paths Omi never takes, plus key generation/signing we
# do not perform. Keeping the list explicit keeps the kernel image small.
BEARSSL_SKIP := aes_big aes_ct64 aes_pwr8 aes_x86ni aes_small ghash_pclmul \
                ghash_pwr8 c25519_m15 c25519_m64 p256_m15 p256_m64 all_m15 \
                all_m64 chacha20_sse2 des_ descbc sysrng pemdec pemenc keygen \
                sign server scert ccert
BEARSSL_SOURCES := $(filter-out $(foreach p,$(BEARSSL_SKIP),$(BEARSSL)/src/*/*$(p)*.c),$(BEARSSL_ALL))

# Gumbo HTML5 parser (vendored, freestanding, kernel heap)
GUMBO_DIR  := kernel/lib/gumbo
GUMBO_ALL  := $(wildcard $(GUMBO_DIR)/src/*.c) $(GUMBO_DIR)/glue.c
GUMBO_SRC  := $(GUMBO_ALL)
GUMBO_OBJ  := $(patsubst $(KERNEL_DIR)/lib/gumbo/%.c,$(BUILD_DIR)/lib/gumbo/%.o,$(GUMBO_SRC))

# The first user-mode program: a static ELF built freestanding and linked at
# the address ring3.c/elf.c map it to. It travels on the ISO as a boot module.
USER_SRC := $(KERNEL_DIR)/user/init.c
USER_LD  := $(KERNEL_DIR)/user/app.ld
USER_ELF := $(BUILD_DIR)/init.elf

OBJECTS := $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES)) \
           $(patsubst $(BEARSSL)/src/%.c,$(BUILD_DIR)/lib/bearssl/%.o,$(BEARSSL_SOURCES)) \
           $(GUMBO_OBJ)

# Limine
LIMINE_VERSION := 8.6.0
LIMINE_DIR := limine
LIMINE_URL := https://github.com/limine-bootloader/limine/releases/download/v$(LIMINE_VERSION)/limine-$(LIMINE_VERSION).tar.xz

# Default target
.PHONY: all
all: $(ISO)

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/boot
	mkdir -p $(BUILD_DIR)/lib
	mkdir -p $(BUILD_DIR)/mm
	mkdir -p $(BUILD_DIR)/fs
	mkdir -p $(BUILD_DIR)/drivers
	mkdir -p $(BUILD_DIR)/gui
	mkdir -p $(BUILD_DIR)/media

# MP3 decoding needs real SSE maths: minimp3 works in float, and the kernel's
# default -mno-sse soft-float ABI does not match compiler-rt / xmm registers.
# Only this file gets SSE; interrupt handlers stay integer-only.
$(BUILD_DIR)/media/mp3.o: $(KERNEL_DIR)/media/mp3.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -msse -msse2 -mfpmath=sse -c $< -o $@

# Gumbo builds with its own port headers (stdint/stddef/stdbool map onto types.h)
# and is third-party code whose warnings are not ours to police.
$(BUILD_DIR)/lib/gumbo/%.o: $(KERNEL_DIR)/lib/gumbo/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -I$(GUMBO_DIR)/port -I$(GUMBO_DIR)/src $(CFLAGS) -w -c $< -o $@

# The renderer includes gumbo.h, so it needs the same port headers the parser
# uses (they keep clang's stdint/stddef/stdbool from colliding with types.h).
$(BUILD_DIR)/gui/omiweb.o: $(KERNEL_DIR)/gui/omiweb.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -I$(GUMBO_DIR)/port -I$(GUMBO_DIR)/src $(CFLAGS) -c $< -o $@

# The user program. Freestanding C, no libc, no relocations, linked by its own
# script (kernel/user/app.ld) at 0x8000000000 - the window elf.c maps.
$(USER_ELF): $(USER_SRC) $(USER_LD) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector \
	      -fno-pic -mno-red-zone -mcmodel=large -O1 -c $(USER_SRC) -o $(BUILD_DIR)/user_init.o
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T $(USER_LD) \
	      -o $@ $(BUILD_DIR)/user_init.o
	@echo "user elf: $$(stat -c %s $@) bytes"

# Every header is a dependency of every object. Crude, but a stale constant in
# an unrebuilt object cost an afternoon once: ring3.h changed the user window and
# elf.o kept spawning tasks onto the old stack address.
KHEADERS := $(wildcard $(KERNEL_DIR)/include/*.h) $(wildcard $(KERNEL_DIR)/include/*/*.h)

# Compile C files
$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c $(KHEADERS) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# BearSSL: freestanding, its own port headers (stdint/stddef map onto types.h),
# and its warning level is not ours to police.
$(BUILD_DIR)/lib/bearssl/%.o: $(BEARSSL)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -I$(BEARSSL)/port -I$(BEARSSL)/inc -I$(BEARSSL)/src -c $< -o $@

# Link kernel
$(KERNEL): $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) $(BUILTINS) -o $@

# Download and build Limine
$(LIMINE_DIR):
	curl -L $(LIMINE_URL) -o limine.tar.xz
	mkdir -p $(LIMINE_DIR)
	tar -xf limine.tar.xz -C $(LIMINE_DIR) --strip-components=1
	rm limine.tar.xz
	cd $(LIMINE_DIR) && ./configure --enable-uefi-x86-64 && make

# Create ISO
# limine.conf is a prerequisite: its module list is part of the ISO's contents,
# and a stale copy on the image looks exactly like a kernel that cannot find a
# module that is sitting right there on the disk.
# The ISO recipe builds userland itself, but each .elf is also a prerequisite so
# that editing a .c rebuilds the image. That needs a rule of its own, or make
# refuses the prerequisite before the recipe ever runs.
userland/%.elf: userland/%.c
	$(MAKE) -C userland $*.elf

$(ISO): $(KERNEL) $(USER_ELF) $(LIMINE_DIR) limine.conf userland/cat.elf userland/httpget.elf userland/polltest.elf userland/spin.elf userland/pipetest.elf userland/linuxtest.elf userland/muslhello.elf userland/libmap.elf userland/dyntest.elf userland/libgreet.so userland/exectest.elf userland/exectarget.elf userland/epolltest.elf userland/memfdtest.elf userland/socktest.elf userland/tlsget.elf userland/surftest.elf userland/omirender.elf userland/surfexit.elf
	@echo "Creating bootable ISO..."
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot
	mkdir -p $(ISO_ROOT)/EFI/BOOT
	mkdir -p $(ISO_ROOT)/limine
	
	# Copy kernel
	cp $(KERNEL) $(ISO_ROOT)/boot/kernel.elf

	# User programs, exposed to the kernel as boot modules
	mkdir -p $(ISO_ROOT)/apps
	cp $(USER_ELF) $(ISO_ROOT)/apps/init.elf

	# Standalone userspace programs: built by their own Makefile with their own
	# flags and linker script, then copied on. Nothing here is linked into the
	# kernel, and nothing there includes a kernel header.
	$(MAKE) -C userland
	cp userland/cat.elf $(ISO_ROOT)/apps/cat.elf
	cp userland/httpget.elf $(ISO_ROOT)/apps/httpget.elf
	cp userland/polltest.elf $(ISO_ROOT)/apps/polltest.elf
	cp userland/pipetest.elf $(ISO_ROOT)/apps/pipetest.elf
	cp userland/linuxtest.elf $(ISO_ROOT)/apps/linuxtest.elf
	@if [ -f userland/muslhello.elf ]; then cp userland/muslhello.elf $(ISO_ROOT)/apps/muslhello.elf; else echo "no muslhello.elf: skipping"; fi
	@if [ -f userland/muslthreads.elf ]; then cp userland/muslthreads.elf $(ISO_ROOT)/apps/muslthreads.elf; else echo "no muslthreads.elf: skipping"; fi
	@if [ -f userland/muslstat.elf ]; then cp userland/muslstat.elf $(ISO_ROOT)/apps/muslstat.elf; else echo "no muslstat.elf: skipping"; fi
	@if [ -f userland/muslfork.elf ]; then cp userland/muslfork.elf $(ISO_ROOT)/apps/muslfork.elf; else echo "no muslfork.elf: skipping"; fi
	@if [ -f userland/cowlives.elf ]; then cp userland/cowlives.elf $(ISO_ROOT)/apps/cowlives.elf; else echo "no cowlives.elf: skipping"; fi
	@if [ -f userland/libprobe.elf ]; then cp userland/libprobe.elf $(ISO_ROOT)/apps/libprobe.elf; else echo "no libprobe.elf: skipping"; fi
	@if [ -f userland/libmap.elf ]; then cp userland/libmap.elf $(ISO_ROOT)/apps/libmap.elf; else echo "no libmap.elf: skipping"; fi
	@if [ -f userland/dyntest.elf ]; then cp userland/dyntest.elf $(ISO_ROOT)/apps/dyntest.elf; else echo "no dyntest.elf: skipping"; fi
	@if [ -f userland/exectest.elf ]; then cp userland/exectest.elf $(ISO_ROOT)/apps/exectest.elf; else echo "no exectest.elf: skipping"; fi
	@if [ -f userland/epolltest.elf ]; then cp userland/epolltest.elf $(ISO_ROOT)/apps/epolltest.elf; else echo "no epolltest.elf: skipping"; fi
	@if [ -f userland/memfdtest.elf ]; then cp userland/memfdtest.elf $(ISO_ROOT)/apps/memfdtest.elf; else echo "no memfdtest.elf: skipping"; fi
	@if [ -f userland/socktest.elf ]; then cp userland/socktest.elf $(ISO_ROOT)/apps/socktest.elf; else echo "no socktest.elf: skipping"; fi
	@if [ -f userland/tlsget.elf ]; then cp userland/tlsget.elf $(ISO_ROOT)/apps/tlsget.elf; else echo "no tlsget.elf: skipping"; fi
	@if [ -f userland/surftest.elf ]; then cp userland/surftest.elf $(ISO_ROOT)/apps/surftest.elf; else echo "no surftest.elf: skipping"; fi
	@if [ -f userland/omirender.elf ]; then cp userland/omirender.elf $(ISO_ROOT)/apps/omirender.elf; else echo "no omirender.elf: skipping"; fi
	@if [ -f userland/surfexit.elf ]; then cp userland/surfexit.elf $(ISO_ROOT)/apps/surfexit.elf; else echo "no surfexit.elf: skipping"; fi
	@if [ -f userland/exectarget.elf ]; then cp userland/exectarget.elf $(ISO_ROOT)/apps/exectarget.elf; else echo "no exectarget.elf: skipping"; fi
	# A shared object, in a directory of its own: the loader will look it up by
	# path, so the path it is copied to is the path that must appear in limine.conf.
	@mkdir -p $(ISO_ROOT)/lib
	@if [ -f userland/libgreet.so ]; then cp userland/libgreet.so $(ISO_ROOT)/lib/libgreet.so; else echo "no libgreet.so: skipping"; fi
	@for m in /lib/ld-musl-x86_64.so.1 /usr/lib/x86_64-linux-musl/libc.so; do \
	  if [ -f "$$m" ]; then cp "$$m" $(ISO_ROOT)/lib/ld-musl-x86_64.so.1; echo "interpreter: $$m"; break; fi; \
	done; [ -f $(ISO_ROOT)/lib/ld-musl-x86_64.so.1 ] || echo "no musl ld.so on this host: dyntest will refuse to load"
	cp userland/spin.elf $(ISO_ROOT)/apps/spin.elf

	# Media samples, exposed to the kernel as boot modules
	mkdir -p $(ISO_ROOT)/media
	cp assets/sample.mp3 assets/sample.mp4 $(ISO_ROOT)/media/

	# TrueType faces, also boot modules
	mkdir -p $(ISO_ROOT)/fonts
	cp assets/fonts/*.ttf $(ISO_ROOT)/fonts/
	
	# Copy Limine config to ALL locations Limine searches
	cp limine.conf $(ISO_ROOT)/limine.conf
	cp limine.conf $(ISO_ROOT)/boot/limine.conf
	cp limine.conf $(ISO_ROOT)/limine/limine.conf
	cp limine.conf $(ISO_ROOT)/EFI/BOOT/limine.conf
	
	# Copy Limine files
	cp $(LIMINE_DIR)/bin/limine-bios.sys $(ISO_ROOT)/boot/ 2>/dev/null || true
	cp $(LIMINE_DIR)/bin/limine-bios-cd.bin $(ISO_ROOT)/boot/ 2>/dev/null || true
	cp $(LIMINE_DIR)/bin/limine-uefi-cd.bin $(ISO_ROOT)/boot/ 2>/dev/null || true
	cp $(LIMINE_DIR)/bin/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	
	# Create ISO using xorriso
	xorriso -as mkisofs -b boot/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		-V OMI -A "Omi" -p "Omi" \
		$(ISO_ROOT) -o $(ISO)
	
	# Install Limine for hybrid boot
	$(LIMINE_DIR)/bin/limine bios-install $(ISO) 2>/dev/null || true
	
	@echo ""
	@echo "============================================"
	@echo "  ISO created: $(ISO)"
	@echo "============================================"
	@echo ""
	@echo "To test in QEMU:"
	@echo "  make run"
	@echo ""
	@echo "To flash to USB (replace /dev/sdX):"
	@echo "  sudo dd if=$(ISO) of=/dev/sdX bs=4M status=progress"
	@echo ""

# Run in QEMU
.PHONY: run
run: $(ISO)
	qemu-system-x86_64 \
		-M q35 \
		-m 512M \
		-cdrom $(ISO) \
		-serial stdio \
		-bios /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
		-vga std

# Run in QEMU with BIOS (fallback)
.PHONY: run-bios
run-bios: $(ISO)
	qemu-system-x86_64 \
		-M q35 \
		-m 512M \
		-cdrom $(ISO) \
		-serial stdio \
		-vga std

# Clean build files
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(ISO_ROOT)
	rm -f $(ISO)

# Deep clean (including Limine)
.PHONY: distclean
distclean: clean
	rm -rf $(LIMINE_DIR)

# Help
.PHONY: help
help:
	@echo "UEFI Demo OS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build the bootable ISO (default)"
	@echo "  run       - Run in QEMU with UEFI"
	@echo "  run-bios  - Run in QEMU with legacy BIOS"
	@echo "  clean     - Remove build files"
	@echo "  distclean - Remove build files and Limine"
	@echo "  help      - Show this help"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - clang or x86_64-elf-gcc"
	@echo "  - ld.lld or x86_64-elf-ld"
	@echo "  - xorriso"
	@echo "  - qemu-system-x86_64 (for testing)"
