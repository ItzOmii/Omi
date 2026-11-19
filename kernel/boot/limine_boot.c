/*
 * UEFI Demo OS - Kernel Entry Point
 * Uses the Limine Boot Protocol for clean 64-bit entry
 */

#include "../include/gui.h"
#include "../include/bt.h"
#include "../include/limine.h"
#include "../include/gdt.h"
#include "../include/idt.h"
#include "../include/task.h"
#include "../include/devboot.h"   /* OMI_DEV_BOOT: 1 spawns the browser and nothing else */
#include "../include/acpi.h"
#include "../include/smp.h"
#include "../include/lapic.h"
#include "../include/percpu.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/limine_modules.h"
#include "../include/mp3.h"
#include "../include/e1000.h"
#include "../include/net.h"
#include "../include/hda.h"
#include "../include/ring3.h"
#include "../include/pmm.h"
#include "../include/elf.h"

/* ========== Limine Requests ========== */

/* Place requests in dedicated section */
__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_requests_start_marker[4] = {0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
                                       0x785c6ed015d3e316, 0x181e920a7852b9d9};

/* Base revision - Use 0 to get full physical memory mapping including device MMIO */
__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[3] = {0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 0};

/* Framebuffer request */
__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
                                  0x9d5827dcd881dd75, 0xa3148604f6fab11b},
                           .revision = 0,
                           .response = NULL};

/* RSDP request (ACPI root pointer) */
__attribute__((used,
               section(".limine_requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {.id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
                           0xc5e77b6b397e7b43, 0x27637845accdcf3c},
                    .revision = 0,
                    .response = NULL};

/* HHDM request (Higher Half Direct Map offset) */
__attribute__((used,
               section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
                           0x48dcf1cb8ad2b852, 0x63984e959a98244b},
                    .revision = 0,
                    .response = NULL};

/* Memory map request (so the GUI can report real installed RAM) */
__attribute__((used,
               section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
                             0x67cf3d9d378a806f, 0xe304acdfc50c3c62},
                      .revision = 0,
                      .response = NULL};

/* Kernel address request (physical/virtual base) */
__attribute__((used,
               section(".limine_requests"))) static volatile struct limine_kernel_address_request
    kernel_address_request = {.id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
                                     0x71ba76863cc55f63, 0xb2644a48c516a487},
                              .revision = 0,
                              .response = NULL};

/* Boot modules: files handed over from the boot medium (see limine.conf) */
__attribute__((used, section(".limine_requests"))) static volatile struct meep_module_request
    module_request = {.id = {0xc7b1dd30df4c8b88, 0x0a82e883a194f07b,
                             LIMINE_MODULE_REQUEST_ID0, LIMINE_MODULE_REQUEST_ID1},
                      .revision = 0,
                      .response = NULL,
                      .internal_module_count = 0,
                      .internal_modules = NULL};

/* Global HHDM offset for drivers to use */
uint64_t hhdm_offset = 0xFFFF800000000000ULL;
uint64_t system_ram_bytes = 0;   /* sum of LIMINE_MEMMAP_USABLE */
uint64_t kernel_phys_base = 0;
uint64_t kernel_virt_base = 0;

/* Debug framebuffer access */
uint32_t *g_fb_ptr = NULL;
uint32_t g_fb_width = 0;
uint32_t g_fb_height = 0;
uint32_t g_fb_pitch = 0;

void debug_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_fb_ptr) return;
  for (int dy = 0; dy < h; dy++) {
    if (y + dy >= (int)g_fb_height) break;
    volatile uint32_t *row = (volatile uint32_t *)((uint8_t *)g_fb_ptr + (y + dy) * g_fb_pitch);
    for (int dx = 0; dx < w; dx++) {
      if (x + dx >= (int)g_fb_width) break;
      row[x + dx] = color;
    }
  }
}

/* Request end marker */
__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_requests_end_marker[2] = {0xadc0e0531bb10d03, 0x9572709f31764c62};

/* ========== Kernel State ========== */

static struct limine_framebuffer *fb = NULL;

/* ========== Early Serial Debug ========== */

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static void serial_init(void) {
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x80);
  outb(COM1 + 0, 0x03);
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x03);
  outb(COM1 + 2, 0xC7);
  outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c) {
  while ((inb(COM1 + 5) & 0x20) == 0)
    ;
  outb(COM1, c);
}

void serial_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      serial_putc('\r');
    serial_putc(*s++);
  }
}

void serial_puthex(uint64_t val) {
  const char *hex = "0123456789ABCDEF";
  serial_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    serial_putc(hex[(val >> i) & 0xF]);
  }
}

/* Parse EDID to get physical size (mm). Returns 0 on success. */
static int edid_get_mm(const uint8_t *edid, uint64_t size,
                       int *out_mm_w, int *out_mm_h) {
  if (!edid || size < 128 || !out_mm_w || !out_mm_h) {
    return -1;
  }
  /* EDID header: 00 FF FF FF FF FF FF 00 */
  if (!(edid[0] == 0x00 && edid[1] == 0xFF && edid[2] == 0xFF &&
        edid[3] == 0xFF && edid[4] == 0xFF && edid[5] == 0xFF &&
        edid[6] == 0xFF && edid[7] == 0x00)) {
    return -1;
  }
  /* Bytes 21/22: horizontal/vertical size in cm */
  int cm_w = edid[21];
  int cm_h = edid[22];
  if (cm_w == 0 || cm_h == 0) {
    return -1;
  }
  *out_mm_w = cm_w * 10;
  *out_mm_h = cm_h * 10;
  return 0;
}

/* ========== Simple Halt ========== */

static void halt(void) {
  for (;;) {
    __asm__ volatile("hlt");
  }
}

/* ========== Direct Screen Test ========== */

/* Draw directly to framebuffer without any library functions */
static void direct_screen_test(void *fb_addr, uint64_t width, uint64_t height,
                               uint64_t pitch) {
  volatile uint8_t *fb = (volatile uint8_t *)fb_addr;

  serial_puts("Drawing test pattern...\n");

  /* Fill entire screen with a gradient - direct pixel writes */
  for (uint64_t y = 0; y < height; y++) {
    volatile uint32_t *row = (volatile uint32_t *)(fb + y * pitch);
    for (uint64_t x = 0; x < width; x++) {
      /* Create a nice gradient: purple to blue */
      uint8_t r = 50;
      uint8_t g = (y * 50 / height) + 20;
      uint8_t b = 100 + (y * 100 / height);
      row[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
  }

  serial_puts("Test pattern complete!\n");

  /* Draw a white rectangle in center as focus point */
  uint64_t cx = width / 2 - 100;
  uint64_t cy = height / 2 - 50;
  for (uint64_t y = cy; y < cy + 100 && y < height; y++) {
    volatile uint32_t *row = (volatile uint32_t *)(fb + y * pitch);
    for (uint64_t x = cx; x < cx + 200 && x < width; x++) {
      row[x] = 0xFFFFFFFF; /* White */
    }
  }

  /* Draw "UEFI-OS" text approximation with colored blocks */
  uint64_t text_y = height / 2 - 20;
  uint64_t text_x = width / 2 - 80;
  volatile uint32_t *text_row = (volatile uint32_t *)(fb + text_y * pitch);

  /* U */
  for (int i = 0; i < 30; i++)
    text_row[text_x + i] = 0xFF00FF00;
  text_x += 35;
  /* E */
  for (int i = 0; i < 25; i++)
    text_row[text_x + i] = 0xFF00FF00;
  text_x += 30;
  /* F */
  for (int i = 0; i < 20; i++)
    text_row[text_x + i] = 0xFF00FF00;
  text_x += 25;
  /* I */
  for (int i = 0; i < 10; i++)
    text_row[text_x + i] = 0xFF00FF00;
  text_x += 15;
  /* - */
  for (int i = 0; i < 15; i++)
    text_row[text_x + i] = 0xFFFFFF00;
  text_x += 20;
  /* O */
  for (int i = 0; i < 25; i++)
    text_row[text_x + i] = 0xFF00FFFF;
  text_x += 30;
  /* S */
  for (int i = 0; i < 20; i++)
    text_row[text_x + i] = 0xFF00FFFF;
}

/* ========== Kernel Main ========== */

/*
 * Enable the FPU/SSE units.
 *
 * We compile the kernel with -mno-sse, but code we link in from compiler-rt
 * (soft-float helpers pulled in by the MP3 decoder) is baseline x86_64 and
 * uses SSE registers. Without CR4.OSFXSR the first such instruction is #UD
 * and the machine triple-faults.
 */
/*
 * compiler-rt is built with stack protection, so its objects read the canary
 * from %fs:0x28. Give the kernel an FS base with a canary in the right slot;
 * otherwise the first soft-float call page-faults at address 0x28.
 */
static uint8_t kernel_fs_area[64] __attribute__((aligned(64)));

static void cpu_setup_fs_canary(void) {
  *(uint64_t *)(kernel_fs_area + 0x28) = 0x9E3779B97F4A7C15ULL;
  uint64_t base = (uint64_t)(uintptr_t)kernel_fs_area;
  uint32_t lo = (uint32_t)base;
  uint32_t hi = (uint32_t)(base >> 32);
  __asm__ volatile("wrmsr" ::"c"(0xC0000100), "a"(lo), "d"(hi)); /* IA32_FS_BASE */
}

static void cpu_enable_fpu_sse(void) {
  uint64_t cr0, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~((uint64_t)1 << 2); /* EM = 0 : no FPU emulation */
  cr0 &= ~((uint64_t)1 << 3); /* TS = 0 : no lazy context switch */
  __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));

  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= ((uint64_t)1 << 9);  /* OSFXSR: enable FXSAVE/FXRSTOR, SSE state */
  cr4 |= ((uint64_t)1 << 10); /* OSXMMEXCPT: enable #XF for SSE faults */
  __asm__ volatile("mov %0, %%cr4" ::"r"(cr4));
  __asm__ volatile("fninit");
}

void _start(void) {
  /* Initialize serial for debug output */
  serial_init();
  serial_puts("\n\n=== UEFI Demo OS ===\n");
  serial_puts("Kernel entry point reached!\n");

  /* FPU/SSE + FS base, before any float (compiler-rt) code can run */
  cpu_enable_fpu_sse();
  cpu_setup_fs_canary();

  /* Initialize IDT/PIC (interrupts still disabled by default) */
  gdt_init();              /* own GDT + TSS before any interrupt */
  idt_init();
  pic_init();
  task_system_start();     /* PIT + IRQ0, scheduler ready */

  /* Verify base revision was accepted */
  if (limine_base_revision[2] != 0) {
    serial_puts("ERROR: Limine base revision mismatch\n");
    serial_puts("Revision value: ");
    serial_puthex(limine_base_revision[2]);
    serial_puts("\n");
    halt();
  }
  serial_puts("Limine base revision OK\n");

  /* Get HHDM offset for physical memory access */
  if (memmap_request.response) {
    struct limine_memmap_response *mr = memmap_request.response;
    for (uint64_t i = 0; i < mr->entry_count; i++) {
      struct limine_memmap_entry *e = mr->entries[i];
      if (e->type == 0) system_ram_bytes += e->length;   /* LIMINE_MEMMAP_USABLE */
    }
  }

  serial_puts("RAM usable: ");
  serial_puthex(system_ram_bytes);
  serial_puts("\n");

  if (hhdm_request.response) {
    hhdm_offset = hhdm_request.response->offset;
    serial_puts("HHDM offset: ");
    serial_puthex(hhdm_offset);
    serial_puts("\n");
  }

  /* Boot modules (sample media) */
  meep_modules_probe((void *)module_request.response);

  /* Get kernel physical/virtual base */
  if (kernel_address_request.response) {
    kernel_phys_base = kernel_address_request.response->physical_base;
    kernel_virt_base = kernel_address_request.response->virtual_base;
    serial_puts("Kernel phys base: ");
    serial_puthex(kernel_phys_base);
    serial_puts("\nKernel virt base: ");
    serial_puthex(kernel_virt_base);
    serial_puts("\n");
  }

  /* Bring the physical page allocator up now that the map, the direct map, the
   * kernel base and the boot modules are all known. */
  pmm_init(memmap_request.response, (void *)module_request.response);
  pmm_report();

  /* Get framebuffer */
  if (framebuffer_request.response == NULL) {
    serial_puts("ERROR: No framebuffer response!\n");
    halt();
  }

  if (framebuffer_request.response->framebuffer_count < 1) {
    serial_puts("ERROR: No framebuffers available!\n");
    halt();
  }

  fb = framebuffer_request.response->framebuffers[0];
  g_fb_ptr = (uint32_t *)fb->address;
  g_fb_width = (uint32_t)fb->width;
  g_fb_height = (uint32_t)fb->height;
  g_fb_pitch = (uint32_t)fb->pitch;

  /* Initialize ACPI if RSDP was provided */
  if (rsdp_request.response && rsdp_request.response->address) {
    acpi_init(rsdp_request.response->address);
  } else {
    acpi_init(NULL);
  }

  /* How many cores this machine is, before anything tries to use them. */
  smp_init();
  lapic_init();

  serial_puts("Framebuffer acquired:\n");
  serial_puts("  Address: ");
  serial_puthex((uint64_t)fb->address);
  serial_puts("\n  Width: ");
  serial_puthex(fb->width);
  serial_puts("\n  Height: ");
  serial_puthex(fb->height);
  serial_puts("\n  Pitch: ");
  serial_puthex(fb->pitch);
  serial_puts("\n  BPP: ");
  serial_puthex(fb->bpp);
  serial_puts("\n");

  /* Try to read physical display size from EDID */
  if (fb->edid && fb->edid_size >= 128) {
    int mm_w = 0, mm_h = 0;
    if (edid_get_mm((const uint8_t *)fb->edid, fb->edid_size, &mm_w, &mm_h) ==
        0) {
      screen_mm_width = mm_w;
      screen_mm_height = mm_h;
      serial_puts("  EDID size (mm): ");
      serial_puthex((uint64_t)mm_w);
      serial_puts(" x ");
      serial_puthex((uint64_t)mm_h);
      serial_puts("\n");
    }
  }

  /* First: Direct screen test to verify framebuffer works */
  serial_puts("Starting direct framebuffer test...\n");
  direct_screen_test(fb->address, fb->width, fb->height, fb->pitch);

  /* Visual checkpoint: RED = starting fb_init */
  {
    volatile uint32_t *row = (volatile uint32_t *)((uint8_t *)fb->address + 10 * fb->pitch);
    for (int i = 0; i < 50; i++) row[10 + i] = 0xFFFF0000;
  }

  serial_puts("Initializing framebuffer...\n");
  fb_init(fb->address, fb->width, fb->height, fb->pitch);

  /* Visual checkpoint: GREEN = fb_init done, starting gui_init */
  {
    volatile uint32_t *row = (volatile uint32_t *)((uint8_t *)fb->address + 10 * fb->pitch);
    for (int i = 0; i < 50; i++) row[70 + i] = 0xFF00FF00;
  }

  /* Before anything else can fault: prove the machinery that will report it. */
  bt_selftest();

  serial_puts("Initializing NIC...\n");
  net_selftest();
  serial_puts("Initializing HDA...\n");
  hda_selftest();

  serial_puts("Initializing GUI...\n");
  meep_mp3_selftest();
  gui_init();

  /* Visual checkpoint: BLUE = gui_init done, starting main loop */
  {
    volatile uint32_t *row = (volatile uint32_t *)((uint8_t *)fb->address + 10 * fb->pitch);
    for (int i = 0; i < 50; i++) row[130 + i] = 0xFF0000FF;
  }

  /* Enable interrupts globally - REQUIRED for USB/PS2 to work! */
  serial_puts("Enabling interrupts...\n");
  __asm__ volatile("sti");

  /* The APIC path, end to end: one interrupt addressed to this core, taken on
   * our own vector. If the count does not move, the register window or the
   * vector is wrong and an AP would never hear a startup IPI either. */
  if (lapic_ready()) {
    lapic_self_ipi(LAPIC_VECTOR_IPI);
    for (int i = 0; i < 2000000 && !lapic_ipi_count(); i++) __asm__ volatile("pause");
    serial_puts("[lapic] self-ipi count=");
    serial_puthex((uint64_t)lapic_ipi_count());
    serial_puts(lapic_ipi_count() ? " taken\n" : " never arrived\n");
  }

  /* The other cores: INIT, SIPI, SIPI, and each one arrives in long mode with a
   * stack of its own. They park; nothing is scheduled on them yet. */
  percpu_init();
  smp_start_aps();
  smp_start_workers();

  task_demo_start();

  /* User mode: one address space and one iretq away from the kernel. "init"
   * runs a normal write/yield/exit path; "probe" reads a kernel address and
   * must be killed for it, without taking the system down. */
  ring3_init();
#if !OMI_DEV_BOOT
  ring3_spawn_elf("init",  "init.elf", 0);
  ring3_spawn_elf("probe", "init.elf", 1);
  ring3_spawn_elf("spin",  "init.elf", 2);
  ring3_spawn_elf("ui",    "init.elf", 3);
  ring3_spawn_elf("shmw",  "init.elf", 4);
  ring3_spawn_elf("shmr",  "init.elf", 5);
  ring3_spawn_elf("vm",    "init.elf", 6);
  ring3_spawn_elf("thr",   "init.elf", 7);
  ring3_spawn_elf("fx",    "init.elf", 8);
  ring3_spawn_elf("vfs",   "init.elf", 9);

  /* The first program that is not part of the kernel build. It gets a real
   * argv, so this also tests the process-entry ABI rather than a mode number. */
  static const char *cat_ok[]  = { "cat", "/hello.txt" };
  static const char *cat_two[] = { "cat", "/nope", "/motd" };
  ring3_spawn_elf_argv("cat",  "cat.elf", 2, cat_ok);
  ring3_spawn_elf_argv("cat2", "cat.elf", 3, cat_two);

  /* resolve -> connect -> send -> recv, all through descriptors. The address is
   * QEMU's slirp gateway for the host, and the port is whatever the test server
   * was started on; with nothing listening this must fail cleanly, not hang. */
  static const char *get[] = { "httpget", "10.0.2.2", "8899", "/a.txt" };
  /* The second loopback client also exercises the dup lifetime rule, so one
   * boot covers overlap, duplicate-and-close, and two real hostnames. */
  static const char *get2[] = { "httpget", "10.0.2.2", "8899", "/b.txt", "dup" };
  ring3_spawn_elf_argv("httpget", "httpget.elf", 4, get);
  /* Two clients at once: separate connections, separate receive buffers. If
   * the two bodies ever appear in each other's output, this is where it shows. */
  ring3_spawn_elf_argv("httpget2", "httpget.elf", 5, get2);
  /* Two different names, resolved back to back and fetched at the same time:
   * the browser's shape, not a demo's. */
  static const char *get3[] = { "httpget", "example.com", "80", "/" };
  static const char *get4[] = { "httpget", "example.org", "80", "/" };
  ring3_spawn_elf_argv("httpget3", "httpget.elf", 4, get3);
  ring3_spawn_elf_argv("httpget4", "httpget.elf", 4, get4);
  /* Exits holding the socket, so the address-space teardown has an endpoint to
   * reclaim: the kernel must destroy it exactly once, or a dead process keeps
   * receiving segments. */
  static const char *get5[] = { "httpget", "10.0.2.2", "8899", "/c.txt", "noclose" };
  ring3_spawn_elf_argv("httpget5", "httpget.elf", 5, get5);
  /* poll() from ring 3: one descriptor that really waits, two watched by a single
   * call, and readiness that stops being claimed once drained. */
  static const char *pt[] = { "polltest", "10.0.2.2", "8899", "/hold.txt" };
  ring3_spawn_elf_argv("polltest", "polltest.elf", 4, pt);
  /* Pipes, from a program that owns the descriptors: readiness, EOF across a
   * dup, and -EPIPE when the reader is gone. */
  ring3_spawn_elf("pipetest", "pipetest.elf", 0);
  /* The other door: the `syscall` instruction with Linux numbers, driven from a
   * program that has no libc to do it for it. */
  ring3_spawn_elf("linuxtest", "linuxtest.elf", 0);
  /* A program musl built, not us: linked at its natural 0x400000, started from a
   * Linux-shaped stack with an auxv, and expecting a libc's worth of syscalls. */
  {
    static const char *const musl_argv[] = { "muslhello", "--from-omi" };
    ring3_spawn_elf_argv("muslhello", "muslhello.elf", 2, musl_argv);
    ring3_spawn_elf("muslthreads", "muslthreads.elf", 0);
    ring3_spawn_elf("muslstat", "muslstat.elf", 0);
    ring3_spawn_elf("muslfork", "muslfork.elf", 0);
    /* fork's reference counts against lifetimes that actually stress them:
     * ancestors dying first, three generations, four children on one page, and
     * a shared segment that must survive all of it. */
    ring3_spawn_elf("cowlives", "cowlives.elf", 0);
    /* Boot modules as files: the read half of the dynamic linker's job. Runs
     * last so the earlier suites' output is not interleaved with a file test. */
    ring3_spawn_elf("libprobe", "libprobe.elf", 0);
    /* File-backed mappings: what the linker does with the file libprobe just
     * proved it could read. */
    ring3_spawn_elf("libmap", "libmap.elf", 0);
  ring3_spawn_elf("dyntest", "dyntest.elf", 0);
  /* execve last: it replaces the running program, so it is the one test whose
   * output can be mistaken for the test that started it. */
  ring3_spawn_elf("exectest", "exectest.elf", 0);
  ring3_spawn_elf("epolltest", "epolltest.elf", 0);
  ring3_spawn_elf("memfdtest", "memfdtest.elf", 0);
  ring3_spawn_elf("socktest", "socktest.elf", 0);
  ring3_spawn_elf("sandtest", "sandtest.elf", 0);
  ring3_spawn_elf("tlsget", "tlsget.elf", 0);
  /* The address window, before anything that allocates from it. */
  ring3_spawn_elf("vmtest", "vmtest.elf", 0);
  ring3_spawn_elf("shmtest", "shmtest.elf", 0);
  ring3_spawn_elf("surfbig", "surfbig.elf", 0);
  ring3_spawn_elf("disk",   "init.elf", 10);
  ring3_spawn_elf("unixpair", "unixpair.elf", 0);
  ring3_spawn_elf("scm",      "scmtest.elf", 0);
  ring3_spawn_elf("unixname", "unixname.elf", 0);
  ring3_spawn_elf("cred",     "credtest.elf", 0);
  ring3_spawn_elf("form",     "formtest.elf", 0);
  ring3_spawn_elf("img",      "imgtest.elf", 0);
  ring3_spawn_elf("mem",      "memtest.elf", 0);
  ring3_spawn_elf("surftest", "surftest.elf", 0);

  /* The screen: the first ring-3 owner of it, which holds it while the boot
   * tests paint, then paints its own phases, then leaves.
   *
   * The order between this and the display server is settled by who asks for
   * the scene first, and it has to be this program: the display server turns
   * the kernel's painting off when it takes the screen, and the boot tests
   * above are still being drawn. So this asks first and the server waits on
   * -EBUSY until it leaves. It used to ask 35 seconds in - after the boot tests
   * were done - and by then the server had taken the scene and this program
   * could only print `FAIL map: -16`. */
  ring3_spawn_elf("scene", "scene.elf", 0);
  /* And then the display server, which waits for the screen to come free and
   * composites every window's pixels itself, with furniture of its own. */
  ring3_spawn_elf("comp", "comp.elf", 0);
  ring3_spawn_elf("omirender", "omirender.elf", 0);
  ring3_spawn_elf("surfexit", "surfexit.elf", 0);
  }
#endif /* !OMI_DEV_BOOT */

  ring3_spawn_elf("ominet", "ominet.elf", 0);
#if OMI_DEV_BOOT
  /* The two suites that own the socket paths the browser depends on, so a dev
   * boot still proves they hold - they need no network and cost a second. */
  ring3_spawn_elf("unixname", "unixname.elf", 0);
  ring3_spawn_elf("cred",     "credtest.elf", 0);
  ring3_spawn_elf("form",     "formtest.elf", 0);
  ring3_spawn_elf("img",      "imgtest.elf", 0);
  ring3_spawn_elf("mem",      "memtest.elf", 0);
#endif

#if OMI_PANIC_DEMO == 1
  /* PANIC_DEMO=1: the report path, end to end, on a real fault. */
  panic_demo();
#elif OMI_PANIC_DEMO == 2
  /* PANIC_DEMO=2: the other entrance - panic() called from kernel code. */
  {
    void panic_selftest_demo(void);
    panic_selftest_demo();
  }
#endif

  serial_puts("Starting main loop...\n");

  /* Main rendering loop */
  gui_main_loop();

  /* Should never reach here */
  halt();
}
