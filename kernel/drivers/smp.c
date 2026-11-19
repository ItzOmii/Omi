/*
 * SMP: how many cores this machine has, and how the others are started.
 *
 * The kernel boots on the bootstrap processor. The others have to be named
 * before they can be used, and the MADT is where the names are: one entry per
 * processor, its APIC id, and whether firmware left it enabled. Starting one is
 * then a fixed sequence - INIT, wait, de-assert, SIPI, SIPI - after which the
 * core is in real mode at the trampoline's page and the trampoline does the rest.
 *
 * The started cores park. Nothing is scheduled on them yet: the scheduler, the
 * allocator and the heap are all written for one core, and letting a second core
 * walk into them would be a race, not a speedup. What they prove is the path -
 * firmware table, APIC, startup IPI, real mode to long mode, and a C function
 * running on another core with its own stack.
 */

#include "../include/smp.h"
#include "../include/acpi.h"
#include "../include/gui.h"
#include "../include/string.h"
#include "../include/lapic.h"
#include "../include/mmio.h"
#include "../include/pmm.h"
#include "../include/idt.h"
#include "../include/gdt.h"
#include "../include/percpu.h"
#include "../include/task.h"
#include "../include/spinlock.h"

#define MADT_TYPE_LAPIC     0
#define MADT_TYPE_X2APIC    9
#define MADT_X2APIC_ENABLED 0x3   /* enabled | online capable */

static int      ncpu;
static uint32_t cpu_apid[SMP_MAX_CPUS];
static uint32_t lapic_phys;
static int      lapic_ok;

/* The trampoline, assembled and linked at 0x8000 and embedded as a blob. It is
 * real mode code a startup IPI jumps into, so it cannot be part of the kernel
 * image: only something at a low physical address, inside one page, will do. */
extern const uint8_t ap_image_start_ro[];
extern const uint8_t ap_image_end_ro[];

#define AP_TRAMP_PHYS  0x8000
#define AP_TRAMP_VEC   0x08            /* 0x08 * 0x1000 = 0x8000 */
#define AP_PARAM_PML4  8               /* offsets inside the trampoline page  */
#define AP_PARAM_STACK 16
#define AP_PARAM_ENTRY 24
#define AP_PARAM_CPU   32
#define AP_PARAM_IDTR  40
#define AP_PARAM_GDTR  50   /* 40+10 = 50: the IDTR is 10 bytes, do not overlap */

extern uint64_t hhdm_offset;

/* 8259 channel 0 is already counting under the timer's control; latching it and
 * watching it wrap measures real milliseconds without reprogramming anything. */
static void pit_delay_ms(int ms) {
  uint32_t want = (uint32_t)(1193 * ms);   /* 1.193182 MHz */
  uint32_t got = 0;
  __asm__ volatile("outb %%al, %%dx" : : "a"((uint8_t)0x00), "d"((uint16_t)0x43));
  uint16_t prev;
  { uint8_t lo, hi;
    __asm__ volatile("inb %%dx, %%al" : "=a"(lo) : "d"((uint16_t)0x40));
    __asm__ volatile("inb %%dx, %%al" : "=a"(hi) : "d"((uint16_t)0x40));
    prev = (uint16_t)(lo | (hi << 8)); }
  while (got < want) {
    uint8_t lo, hi;
    __asm__ volatile("outb %%al, %%dx" : : "a"((uint8_t)0x00), "d"((uint16_t)0x43));
    __asm__ volatile("inb %%dx, %%al" : "=a"(lo) : "d"((uint16_t)0x40));
    __asm__ volatile("inb %%dx, %%al" : "=a"(hi) : "d"((uint16_t)0x40));
    uint16_t now = (uint16_t)(lo | (hi << 8));
    got += (uint32_t)(uint16_t)(prev - now);
    prev = now;
  }
}

static volatile int ap_started[SMP_MAX_CPUS];

/* ---------------------------------------------------- allocator under contention
 *
 * The bitmap allocator had no lock, so two cores scanning it at once could both
 * see a clear bit, both claim it, and both be handed the same frame. The check
 * is direct: every core stamps each page it is given with its own APIC id and
 * the sequence number, and the bootstrap processor then asks whether any frame
 * was handed out twice and whether every stamp still names its owner.
 */
#define STRESS_PAGES 64
static uint64_t stress_page[SMP_MAX_CPUS][STRESS_PAGES];
static volatile int stress_done;
static uint64_t ap_stack_top[SMP_MAX_CPUS];

static int stress_alloc(uint64_t cpu) {
  uint32_t id = lapic_this_id();
  int n = 0;
  for (int k = 0; k < STRESS_PAGES; k++) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) break;
    stress_page[cpu][k] = phys;
    *(volatile uint64_t *)(uintptr_t)(hhdm_offset + phys) =
        ((uint64_t)id << 32) | (uint64_t)k;
    n++;
  }
  __atomic_add_fetch(&stress_done, 1, __ATOMIC_SEQ_CST);
  return n;
}

/* Runs on the bootstrap processor once every AP has finished. */
/* ------------------------------------------------------------ a thread on a core
 *
 * The proof this test has to give is not "the counter moved" - the bootstrap
 * processor moving it would pass that - but "this core moved it". So the worker
 * records its own APIC id when it starts, and the check compares that against
 * the core it was pinned to. A worker that ran on the BSP records 0 and fails.
 */
#define WORKER_ITERS 400
static volatile uint64_t worker_hits[SMP_MAX_CPUS];
static volatile uint32_t worker_apic[SMP_MAX_CPUS];
static volatile int worker_finished;

static void ap_worker(void) {
  uint32_t id = lapic_this_id();
  worker_apic[id] = id;
  for (int i = 0; i < WORKER_ITERS; i++) {
    worker_hits[id]++;
    task_yield();
  }
  __atomic_add_fetch(&worker_finished, 1, __ATOMIC_SEQ_CST);
  /* Done. A task cannot return into nothing, and a core with nothing to run has
   * no business spinning: halt until an interrupt wakes it (today that is the
   * next stage's IPI, so this core is simply asleep). */
  percpu_set_idle(1);
  for (;;) __asm__ volatile("sti; hlt");
}

static void stress_report(void) {
  for (int spins = 0; spins < 2000 && stress_done < ncpu - 1; spins++) pit_delay_ms(1);

  int total = 0, dup = 0, bad_stamp = 0, cores = 0;
  for (int c = 1; c < ncpu; c++) {
    if (!stress_page[c][0]) continue;
    cores++;
    for (int k = 0; k < STRESS_PAGES && stress_page[c][k]; k++) {
      total++;
      uint32_t id = cpu_apid[c];
      uint64_t expect = ((uint64_t)id << 32) | (uint64_t)k;
      if (*(volatile uint64_t *)(uintptr_t)(hhdm_offset + stress_page[c][k]) != expect)
        bad_stamp++;
      for (int c2 = 1; c2 <= c; c2++) {
        int lim = (c2 == c) ? k : STRESS_PAGES;
        for (int k2 = 0; k2 < lim; k2++)
          if (stress_page[c2][k2] == stress_page[c][k]) dup++;
      }
    }
  }
  serial_puts("[pmm] stress: ");
  serial_puthex((uint64_t)total);
  serial_puts(" pages from ");
  serial_puthex((uint64_t)cores);
  serial_puts(" cores, duplicates=");
  serial_puthex((uint64_t)dup);
  serial_puts(" bad_stamps=");
  serial_puthex((uint64_t)bad_stamp);
  serial_puts(dup == 0 && bad_stamp == 0 && cores == ncpu - 1 ? "  ok\n" : "  FAILED\n");

  /* Hand the frames back: the point was the check, not the memory. */
  for (int c = 1; c < ncpu; c++)
    for (int k = 0; k < STRESS_PAGES && stress_page[c][k]; k++) {
      pmm_free_page(stress_page[c][k]);
      stress_page[c][k] = 0;
    }
}

/* What an application processor runs once it is in long mode: the same page
 * tables as every other core, a stack of its own, and nothing shared written
 * except its own flag. */
void smp_ap_main(uint64_t cpu) {
  ap_started[cpu] = 1;
  serial_puts("[ap] cpu=");
  serial_puthex(cpu);
  serial_puts(" apic=");
  serial_puthex((uint64_t)lapic_this_id());
  serial_puts(" online, ");
  {
    int n = stress_alloc(cpu);
    serial_puts("allocated ");
    serial_puthex((uint64_t)n);
    serial_puts(" pages\n");
  }

  /* From here this core schedules. The idle slot owns the trampoline stack it is
   * already running on, so the first yield saves a frame that belongs to a task
   * the core can be switched back into. */
  int idle = task_spawn_ap_idle(lapic_this_id(), ap_stack_top[cpu]);
  if (idle < 0) {
    serial_puts("[ap] no room for an idle task, parking\n");
    for (;;) __asm__ volatile("cli; hlt");
  }
  sched_ap_enter(idle);
}

static int start_one_ap(int i) {
  uint8_t *tramp = (uint8_t *)(uintptr_t)(hhdm_offset + AP_TRAMP_PHYS);
  uint64_t stack_phys = pmm_alloc_page();
  if (!stack_phys) { serial_puts("[smp] no page for an AP stack\n"); return 0; }

  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  *(uint64_t *)(tramp + AP_PARAM_PML4)  = cr3 & ~0xFFFULL;
  ap_stack_top[i] = hhdm_offset + stack_phys + 0x1000;
  *(uint64_t *)(tramp + AP_PARAM_STACK) = ap_stack_top[i];
  *(uint64_t *)(tramp + AP_PARAM_ENTRY) = (uint64_t)(uintptr_t)&smp_ap_main;
  *(uint64_t *)(tramp + AP_PARAM_CPU)   = (uint64_t)i;
  {
    idt_ptr_t idtr;
    idt_get_idtr(&idtr);
    memcpy(tramp + AP_PARAM_IDTR, &idtr, 10);
    gdt_get_gdtr(tramp + AP_PARAM_GDTR);
  }

  uint32_t id = smp_cpu_apic_id(i);
  lapic_send_init(id);
  pit_delay_ms(10);
  lapic_send_init_deassert(id);
  pit_delay_ms(1);
  lapic_send_startup(id, AP_TRAMP_VEC);
  pit_delay_ms(2);                     /* 200us is the spec; be generous */
  lapic_send_startup(id, AP_TRAMP_VEC);
  pit_delay_ms(10);

  return ap_started[i] != 0;
}

/* Spawn one worker per AP, all pinned, and wait for each to finish. */
void smp_start_workers(void) {
  if (ncpu < 2) return;
  int want = 0;
  for (int i = 1; i < ncpu; i++)
    if (task_spawn_on("ap-worker", ap_worker, cpu_apid[i]) >= 0) want++;
  if (!want) { serial_puts("[smp] no worker could be spawned\n"); return; }

  for (int spins = 0; spins < 8000 && worker_finished < want; spins++) pit_delay_ms(1);

  for (int i = 1; i < ncpu; i++) {
    uint32_t id = cpu_apid[i];
    int ok = worker_apic[id] == id && worker_hits[id] == WORKER_ITERS;
    serial_puts("[smp] worker pinned=");
    serial_puthex(id);
    serial_puts(" ran_on=");
    serial_puthex(worker_apic[id]);
    serial_puts(" iters=");
    serial_puthex(worker_hits[id]);
    serial_puts(ok ? "  ok\n" : "  FAILED\n");
  }
}

void smp_start_aps(void) {
  if (ncpu <= 1) { serial_puts("[smp] nothing to start\n"); return; }
  if (!lapic_ready()) { serial_puts("[smp] no local APIC: cannot start APs\n"); return; }

  /* Paging comes on inside the trampoline, and the instruction after CR0.PG is
   * fetched at 0x8000 - so that page has to be mapped where it is. */
  if (!mmio_map_at(AP_TRAMP_PHYS, AP_TRAMP_PHYS, 0x1000)) {
    serial_puts("[smp] could not identity-map the trampoline page\n");
    return;
  }
  uint8_t *dst = (uint8_t *)(uintptr_t)(hhdm_offset + AP_TRAMP_PHYS);
  size_t len = (size_t)(ap_image_end_ro - ap_image_start_ro);
  if (len > 0x1000) { serial_puts("[smp] trampoline is bigger than a page\n"); return; }
  memcpy(dst, ap_image_start_ro, len);

  int ok = 0;
  for (int i = 1; i < ncpu; i++) {
    if (start_one_ap(i)) {
      ok++;
    } else {
      serial_puts("[smp] cpu ");
      serial_puthex((uint64_t)i);
      serial_puts(" never answered\n");
    }
  }

  serial_puts("[smp] started ");
  serial_puthex((uint64_t)ok);
  serial_puts(" of ");
  serial_puthex((uint64_t)(ncpu - 1));
  serial_puts(" application processors\n");

  if (ok) stress_report();
}

void smp_init(void) {
  ncpu = 0;
  lapic_phys = 0;
  lapic_ok = 0;

  const acpi_madt_t *m = acpi_get_madt();
  if (!m) {
    serial_puts("[smp] no MADT: one core, and no way to name another\n");
    return;
  }
  lapic_phys = m->lapic_addr;
  lapic_ok = (m->flags & 1) != 0;          /* PCAT_COMPAT: a 8259 is also there */

  const uint8_t *p = (const uint8_t *)m + sizeof(acpi_madt_t);
  const uint8_t *end = (const uint8_t *)m + m->header.length;
  int seen = 0, disabled = 0;

  while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
    uint8_t type = p[0];
    uint8_t len = p[1];
    if (type == MADT_TYPE_LAPIC && len >= 8) {
      uint32_t apid = p[3];
      uint32_t flags = *(const uint32_t *)(p + 4);
      seen++;
      if ((flags & 1) && ncpu < SMP_MAX_CPUS) cpu_apid[ncpu++] = apid;
      else if (!(flags & 1)) disabled++;
    } else if (type == MADT_TYPE_X2APIC && len >= 12) {
      uint32_t apid = *(const uint32_t *)(p + 4);
      uint32_t flags = *(const uint32_t *)(p + 8);
      seen++;
      if ((flags & MADT_X2APIC_ENABLED) && ncpu < SMP_MAX_CPUS) cpu_apid[ncpu++] = apid;
    }
    p += len;
  }

  serial_puts("[smp] cpus=");
  serial_puthex((uint64_t)ncpu);
  serial_puts(" of ");
  serial_puthex((uint64_t)seen);
  if (disabled) { serial_puts(" (disabled "); serial_puthex((uint64_t)disabled); serial_puts(")"); }
  serial_puts(" apic_ids=");
  for (int i = 0; i < ncpu; i++) {
    if (i) serial_puts(",");
    serial_puthex((uint64_t)cpu_apid[i]);
  }
  serial_puts(" lapic=");
  serial_puthex((uint64_t)lapic_phys);
  serial_puts(" booting_on_apic=");
  serial_puthex((uint64_t)cpu_apid[0]);
  serial_puts("\n");
}

int      smp_cpu_count(void)         { return ncpu; }
uint32_t smp_cpu_apic_id(int i)      { return (i >= 0 && i < ncpu) ? cpu_apid[i] : 0xFFFFFFFFu; }
uint32_t smp_lapic_addr(void)        { return lapic_phys; }
int      smp_lapic_present(void)     { return lapic_ok; }