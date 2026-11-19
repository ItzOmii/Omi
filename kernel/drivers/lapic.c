/*
 * Local APIC driver: xAPIC register window, end-of-interrupt, IPIs.
 *
 * The registers are memory-mapped at the address the MADT advertises (0xFEE00000
 * on every machine that has one). That address is not RAM, so it is not in the
 * direct map: it is mapped explicitly, the same way a PCI BAR is.
 */

#include "../include/lapic.h"
#include "../include/mmio.h"
#include "../include/smp.h"
#include "../include/gui.h"
#include "../include/idt.h"
#include "../include/string.h"

#define IA32_APIC_BASE   0x1B

#define LAPIC_ID         0x20
#define LAPIC_VER        0x30
#define LAPIC_TPR        0x80
#define LAPIC_EOI        0xB0
#define LAPIC_SVR        0xF0
#define LAPIC_ESR        0x280
#define LAPIC_ICR_LO     0x300
#define LAPIC_ICR_HI     0x310

#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_X2APIC (1ULL << 10)
#define SVR_ENABLE       0x100
#define SVR_SPURIOUS     0xFF

static volatile uint32_t *r = NULL;      /* register window */
static uint32_t g_base, g_id, g_ver, g_ipis;
static int g_ready, g_x2apic;

static uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t v) {
  __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void puthex(const char *label, uint64_t v) {
  serial_puts(label);
  serial_puthex(v);
}

/* Take an interrupt on our own vector and believe the count only if it moved. */
static void ipi_handler(interrupt_frame_t *frame) {
  (void)frame;
  g_ipis++;
}

void lapic_init(void) {
  g_ready = 0; g_x2apic = 0; g_ipis = 0;

  if (!smp_lapic_present()) {
    serial_puts("[lapic] MADT does not advertise a local APIC\n");
    return;
  }
  g_base = smp_lapic_addr();
  if (!g_base) g_base = 0xFEE00000;

  uint64_t base_msr = rdmsr(IA32_APIC_BASE);
  if (base_msr & APIC_BASE_X2APIC) {
    /* x2APIC speaks through MSRs, not MMIO. The ids still come from the MADT, so
     * a machine that boots in this mode is usable, but not by this driver yet. */
    g_x2apic = 1;
    serial_puts("[lapic] report: firmware left the APIC in x2APIC mode\n");
    return;
  }
  if (!(base_msr & APIC_BASE_ENABLE)) {
    wrmsr(IA32_APIC_BASE, base_msr | APIC_BASE_ENABLE);
    serial_puts("[lapic] global enable was clear; set\n");
  }

  uint64_t va = mmio_map_range((uint64_t)g_base, 0x1000);
  if (!va) { serial_puts("[lapic] could not map the register window\n"); return; }
  r = (volatile uint32_t *)(uintptr_t)va;

  g_ver = (r[LAPIC_VER / 4] >> 0) & 0xFF;
  g_id  = r[LAPIC_ID / 4] >> 24;

  /* Spurious vector must be programmed before the enable bit is set. 0xFF is the
   * convention; bit 8 turns the unit on and raises the task priority above the
   * reset value of 0. */
  r[LAPIC_TPR / 4] = 0;
  r[LAPIC_SVR / 4] = SVR_ENABLE | SVR_SPURIOUS;
  r[LAPIC_ESR / 4] = 0;                    /* write-1-to-clear the error status */
  r[LAPIC_ESR / 4] = 0;

  g_ready = 1;
  puthex("[lapic] base=0x", g_base);
  puthex(" id=", g_id);
  puthex(" ver=", g_ver);
  puthex(" booting_on_apic=", smp_cpu_apic_id(0));
  serial_puts(g_id == smp_cpu_apic_id(0) ? " (matches MADT)\n" : " (MADT and APIC disagree)\n");
}

void lapic_eoi(void) {
  if (r) r[LAPIC_EOI / 4] = 0;
}

void lapic_self_ipi(uint8_t vector) {
  if (!r) return;
  idt_register_handler(vector, ipi_handler);
  /* destination 0 in the high word means "this core", which is what a self-IPI
   * is: the same code path an IPI to another core takes. */
  r[LAPIC_ICR_HI / 4] = 0;
  r[LAPIC_ICR_LO / 4] = vector;
}

/* Wait for the ICR to drain: hardware clears the delivery-status bit when the
 * interrupt has actually been handed to the bus, and a second write before that
 * is dropped silently. */
static int icr_idle(void) {
  for (int i = 0; i < 1000000; i++)
    if (!(r[LAPIC_ICR_LO / 4] & (1u << 12))) return 1;
  return 0;
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
  if (!r) return;
  if (!icr_idle()) { serial_puts("[lapic] ICR never drained\n"); return; }
  r[LAPIC_ICR_HI / 4] = apic_id << 24;
  r[LAPIC_ICR_LO / 4] = vector;            /* fixed delivery, physical dest */
}

/* The id register is per-core, so any core can ask who it is. */
uint32_t lapic_this_id(void) {
  return r ? (r[LAPIC_ID / 4] >> 24) : 0xFFFFFFFFu;
}

/* Delivery modes in bits 10:8, per the SDM: 100 is NMI, 101 is INIT, 110 is
 * startup. Getting them the wrong way round is not subtle - the mistake reads as
 * "send an NMI", which reset the machine - but it is silent to the compiler, so
 * the values are spelled out here rather than OR-ed together at the call site. */
#define ICR_LEVEL_TRIGGER 0x00008000u
#define ICR_LEVEL_ASSERT  0x00004000u
#define ICR_MODE_INIT     0x00000500u
#define ICR_MODE_STARTUP  0x00000600u

void lapic_send_init(uint32_t apic_id) {
  if (!r || !icr_idle()) return;
  r[LAPIC_ICR_HI / 4] = apic_id << 24;
  r[LAPIC_ICR_LO / 4] = ICR_LEVEL_TRIGGER | ICR_LEVEL_ASSERT | ICR_MODE_INIT;
}

/* A level-triggered INIT stays asserted until it is de-asserted, and a core held
 * with INIT asserted ignores the SIPI that follows it. */
void lapic_send_init_deassert(uint32_t apic_id) {
  if (!r || !icr_idle()) return;
  r[LAPIC_ICR_HI / 4] = apic_id << 24;
  r[LAPIC_ICR_LO / 4] = ICR_LEVEL_TRIGGER | ICR_MODE_INIT;
}

/* Startup: the vector names a page, and the core begins executing in real mode at
 * vector*0x1000:0000, so it must be below 1 MB. */
void lapic_send_startup(uint32_t apic_id, uint8_t vector) {
  if (!r || !icr_idle()) return;
  r[LAPIC_ICR_HI / 4] = apic_id << 24;
  r[LAPIC_ICR_LO / 4] = ICR_LEVEL_TRIGGER | ICR_LEVEL_ASSERT | ICR_MODE_STARTUP | (uint32_t)vector;
}

/* Readback for bring-up work: what the unit thinks happened to the last send. */
uint32_t lapic_icr_low(void)  { return r ? r[LAPIC_ICR_LO / 4] : 0; }
uint32_t lapic_icr_high(void) { return r ? r[LAPIC_ICR_HI / 4] : 0; }
uint32_t lapic_esr(void)      { uint32_t v = r ? r[LAPIC_ESR / 4] : 0; if (r) { r[LAPIC_ESR / 4] = 0; r[LAPIC_ESR / 4] = 0; } return v; }

int      lapic_ready(void)     { return g_ready; }
uint32_t lapic_id(void)        { return g_id; }
uint32_t lapic_version(void)   { return g_ver; }
uint32_t lapic_base(void)      { return g_base; }
uint32_t lapic_ipi_count(void) { return g_ipis; }